#include "Psram.hpp"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/psram.h"
#include "pico/error.h"

#include <cstdint>
#include <cstdlib>

namespace {

// PSRAM (QMI CS1) maps at XIP_BASE + the board's flash size, per pico-sdk's
// pico_psram_region.template.ld (PSRAM origin 0x11000000) -- same offset as
// psram_check_address() derives from FLASH_DEVINFO_SIZE_MAX. This board's
// PICO_FLASH_SIZE_BYTES is 16MB, so the fixed origin is 0x10000000+16MB =
// 0x11000000 regardless of the chip's detected size. Hardcoded: the SDK
// doesn't expose that size API publicly, and the flash size is fixed anyway.
constexpr uintptr_t kPsramBase = 0x11000000;
constexpr uint8_t kPsramCsPin = PICO_PSRAM_CS_PIN;

// hardware_psram's default 133MHz is the chip's datasheet max at 3.3V, not
// necessarily safe for this board's hand-wired GPIO47. 30MHz actually changes
// the bus speed, to tell whether this is a signal-integrity problem. This is a
// diagnostic step, not final (see the rxdelay clamp below for the real fix).
constexpr uint32_t kPsramMaxFreqHz = 30'000'000;

// Same reasoning: hardware_psram's default 18ns deselect is shorter than the
// PSRAM datasheet's real 50ns minimum.
constexpr uint32_t kPsramMinDeselectNs = 50;

// Sampled (not byte-exhaustive) read/write self-test: two independent
// patterns per sample point (an address-derived byte and its complement)
// spread across the full range, so a stuck-at line fault is very likely
// caught.
constexpr size_t kSelfTestSampleCount = 64;

bool run_self_test(uint8_t* base, size_t size, PsramStatus& status) {
    for (size_t i = 0; i < kSelfTestSampleCount; ++i) {
        size_t offset = (size / kSelfTestSampleCount) * i;
        volatile uint8_t* p = base + offset;
        uint8_t pattern = static_cast<uint8_t>(offset ^ 0x55);
        *p = pattern;
        uint8_t read_back = *p;
        if (read_back != pattern) {
            status.fail_offset = offset;
            status.fail_expected = pattern;
            status.fail_actual = read_back;
            status.fail_on_second_pattern = false;
            return false;
        }
        uint8_t inverted = static_cast<uint8_t>(~pattern);
        *p = inverted;
        read_back = *p;
        if (read_back != inverted) {
            status.fail_offset = offset;
            status.fail_expected = inverted;
            status.fail_actual = read_back;
            status.fail_on_second_pattern = true;
            return false;
        }
    }
    return true;
}

// First-fit free-list allocator state -- O(n) walk is fine: construction/
// destruction happens a handful of times, not per-frame. Each block is
// prefixed by one Block header; `size` is payload size, excluding it.
struct Block {
    size_t size;
    bool free;
    Block* next;
};

PsramStatus g_status;
Block* g_free_list = nullptr;

constexpr size_t align_up(size_t n, size_t align) { return (n + align - 1) & ~(align - 1); }

} // namespace

PsramStatus psram_hw_init() {
    g_status = PsramStatus{};
    g_free_list = nullptr;

    uint8_t cs_pins[] = {kPsramCsPin};
    size_t size = psram_detect_cs_and_size(cs_pins, 1);
    if (size == 0) {
        return g_status; // not present (or not responding on this CS pin)
    }
    g_status.present = true;
    g_status.size_bytes = size;

    // psram_detect_size() restores flash_devinfo's CS1 size to its pre-probe
    // value once done (it only needed a placeholder to make the ID-read
    // addressable). On a fresh boot that's FLASH_DEVINFO_SIZE_NONE, so without
    // restoring it here psram_reinitialize() would configure the QMI CS1
    // window with a registered size of zero, silently "succeeding" with a
    // zero-size window: every access reads back 0 (writes dropped).
    flash_devinfo_set_cs_size(1, flash_devinfo_bytes_to_size(static_cast<uint32_t>(size)));

    g_status.clk_sys_hz_at_test = clock_get_hz(clk_sys);

    // psram_configure_params() computes `divisor = ceil(clk_sys/max_freq)` and
    // sets `rxdelay = divisor` (occasionally +1) -- but QMI_M1_TIMING_RXDELAY
    // is a 3-bit field (max 7), so an out-of-range rxdelay is silently
    // truncated, configuring wrong receive timing. Clamp the *effective* max
    // frequency so the divisor can never exceed 7 whatever clk_sys is.
    constexpr uint32_t kMaxRxdelayDivisor = 7; // QMI_M1_TIMING_RXDELAY_BITS' field width (3 bits)
    uint32_t min_freq_for_divisor_limit = g_status.clk_sys_hz_at_test / kMaxRxdelayDivisor + 1;
    uint32_t max_freq_hz = kPsramMaxFreqHz > min_freq_for_divisor_limit ? kPsramMaxFreqHz : min_freq_for_divisor_limit;

    if (psram_configure_params(max_freq_hz, PICO_DEFAULT_PSRAM_MAX_SELECT, kPsramMinDeselectNs) != PICO_OK) {
        return g_status; // present, but couldn't compute valid timing params -- test_ok stays false
    }
    if (psram_reinitialize() != PICO_OK) {
        return g_status; // present, but QMI CS1 setup failed
    }

    auto* base = reinterpret_cast<uint8_t*>(kPsramBase);
    if (!run_self_test(base, size, g_status)) {
        return g_status; // detected but failed the read/write test -- see fail_* fields for diagnostics
    }

    g_status.test_ok = true;

    g_free_list = reinterpret_cast<Block*>(base);
    g_free_list->size = size - sizeof(Block);
    g_free_list->free = true;
    g_free_list->next = nullptr;
    return g_status;
}

const PsramStatus& psram_status() { return g_status; }

extern "C" void* psram_malloc(size_t size) {
    if (!g_status.test_ok || size == 0) return nullptr;
    size = align_up(size, 8);

    for (Block* b = g_free_list; b != nullptr; b = b->next) {
        if (!b->free || b->size < size) continue;
        if (b->size >= size + sizeof(Block) + 8) {
            auto* remainder = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(b) + sizeof(Block) + size);
            remainder->size = b->size - size - sizeof(Block);
            remainder->free = true;
            remainder->next = b->next;
            b->next = remainder;
            b->size = size;
        }
        b->free = false;
        return reinterpret_cast<uint8_t*>(b) + sizeof(Block);
    }
    return nullptr; // OOM
}

size_t psram_used_bytes() {
    if (!g_status.test_ok) return 0;
    size_t free_bytes = 0;
    for (Block* b = g_free_list; b != nullptr; b = b->next) {
        if (b->free) free_bytes += b->size;
    }
    return g_status.size_bytes - free_bytes;
}

extern "C" void psram_free(void* ptr) {
    if (!ptr) return;
    auto* b = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(ptr) - sizeof(Block));
    b->free = true;

    // Coalesce adjacent free blocks: the list is built address-ascending and
    // splits insert the remainder right after its parent, so list order
    // matches address order -- a plain "merge with next while touching" walk
    // suffices, no sorting needed.
    for (Block* cur = g_free_list; cur != nullptr && cur->next != nullptr;) {
        bool adjacent =
            reinterpret_cast<uint8_t*>(cur) + sizeof(Block) + cur->size == reinterpret_cast<uint8_t*>(cur->next);
        if (cur->free && cur->next->free && adjacent) {
            cur->size += sizeof(Block) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}