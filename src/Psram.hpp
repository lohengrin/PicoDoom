#pragma once
#include <cstddef>
#include <cstdint>

// PSRAM bring-up for the Waveshare RP2350-PiZero's 8MB QPSI PSRAM (chip
// select on GPIO47, see boards/waveshare_rp2350_pizero.h's PICO_PSRAM_CS_PIN).
// Built on pico-sdk 2.3.0's hardware_psram driver (detect/configure/memory-
// map via QMI CS1); this file adds the self-test and a malloc/free heap over
// the mapped region. psram_hw_init() must be called once, from core0, before
// any psram_malloc() and before core1 (if any) starts.

struct PsramStatus {
    bool present = false;   // chip detected (responded to the QSPI ID read)
    bool test_ok = false;   // detected AND the read/write self-test passed
    size_t size_bytes = 0;  // 0 if not present

    // Diagnostics for the first failing sample, valid only when `present`
    // and !`test_ok` -- see Psram.cpp's run_self_test().
    size_t fail_offset = 0;
    uint8_t fail_expected = 0;
    uint8_t fail_actual = 0;
    bool fail_on_second_pattern = false;

    // clk_sys (Hz) when psram_configure_params() ran -- printed on failure
    // since the QMI clock divisor derives from it.
    uint32_t clk_sys_hz_at_test = 0;
};

PsramStatus psram_hw_init();
const PsramStatus& psram_status();

// First-fit free-list allocator over the mapped region. Returns nullptr if
// PSRAM isn't present/healthy, on OOM, or for size 0. Blocks are 8-byte
// aligned. C linkage so the C engine (doom/i_system.c) can call it.
extern "C" void* psram_malloc(size_t size);
extern "C" void psram_free(void* ptr);

size_t psram_used_bytes();