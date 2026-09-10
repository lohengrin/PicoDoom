#include "Ili9486Display.hpp"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include <array>
#include <cstdio>

// Ported verbatim from TOM6809's validated Ili9486Display.cpp (same board +
// panel) -- see that project's file for the full bring-up history behind
// these specific values (shift-register wire protocol, panel-specific init
// register sequence, SPI timing gotchas).
namespace {
constexpr uint kPinSck = 10;
constexpr uint kPinMosi = 11;
constexpr uint kPinMiso = 12; // wired to the touch controller only -- see header doc comment
constexpr uint kPinCs = 8;
constexpr uint kPinDc = 24;
constexpr uint kPinRst = 25;

// Command/parameter clock: 8MHz, matching the vendor reference driver.
// Everything here goes through a shift register (see header), so the usable
// clock is well below a directly-wired ILI9486's.
constexpr uint32_t kCommandBaud = 8 * 1000 * 1000;

// Bulk pixel-streaming clock -- THE knob for refresh speed. Pixel data is a
// plain continuous CS-low burst, so it doesn't depend on the per-word CS
// latching constraining the command path and tolerates a higher clock.
//
// spi_set_baudrate() only ever picks a rate <= what's requested (PL022's
// prescale is even-only, 2-254, times postdiv 1-256), never rounds up. This
// board's clk_peri runs at 150MHz (RP2350 default, not RP2040's 125MHz), so
// the achievable steps near 24MHz are 150/8=18.75MHz and 150/6=25MHz --
// nothing in between. Requesting 24MHz silently landed on 18.75 (confirmed
// via set_window()'s one-shot spi_get_baudrate() log), ~33% slower than
// necessary. 25MHz is only ~4% above TOM6809's tested-safe 24MHz ceiling on
// this same panel; if pixels tear/corrupt at this rate, drop back to
// something that resolves to the 18.75MHz step instead (e.g. 20MHz).
constexpr uint32_t kPixelBaud = 25 * 1000 * 1000;

// ILI9486 command bytes used here.
constexpr uint8_t kCmdSleepOut = 0x11;
constexpr uint8_t kCmdDisplayOn = 0x29;
constexpr uint8_t kCmdCaset = 0x2A;
constexpr uint8_t kCmdPaset = 0x2B;
constexpr uint8_t kCmdRamwr = 0x2C;
constexpr uint8_t kCmdMadctl = 0x36;
constexpr uint8_t kCmdDisFunCtrl = 0xB6;

// MADCTL/Display-Function-Control pair for landscape (480x320). MADCTL 0x28
// is MV=1 (row/column exchange -> landscape) + BGR=1; if red/blue come out
// swapped, clear BGR (0x28 -> 0x20).
constexpr uint8_t kMadctlLandscape = 0x28;
constexpr uint8_t kDisFunLandscape = 0x62;

/// One entry of the power-on register sequence: a command plus its data
/// bytes. Transcribed verbatim from the vendor reference driver's
/// LCD_InitReg() (Waveshare 3.5inch ILI9486 sample code) -- panel-specific
/// tuning, not generic ILI9486 defaults.
struct InitCommand {
    uint8_t cmd;
    uint8_t len;
    std::array<uint8_t, 15> data;
};

constexpr std::array<InitCommand, 16> kInitSequence = {{
    {0xF9, 2, {0x00, 0x08}},
    {0xC0, 2, {0x19, 0x1A}},                   // VREG1OUT positive / VREG2OUT negative
    {0xC1, 2, {0x45, 0x00}},                   // VGH/VGL, VGH >= 14V
    {0xC2, 1, {0x33}},                         // normal-mode drive strength
    {0xC5, 2, {0x00, 0x28}},                   // VCM_REG[7:0], must stay <= 0x80
    {0xB1, 2, {0xA0, 0x11}},                   // frame rate: 0xA0 = 62Hz
    {0xB4, 1, {0x02}},                         // 2-dot frame mode (needs F <= 70Hz)
    {0xB6, 3, {0x00, 0x42, 0x3B}},             // display function control (re-set for orientation below)
    {0xB7, 1, {0x07}},
    {0xE0, 15, {0x1F, 0x25, 0x22, 0x0B, 0x06, 0x0A, 0x4E, 0xC6, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE1, 15, {0x1F, 0x3F, 0x3F, 0x0F, 0x1F, 0x0F, 0x46, 0x49, 0x31, 0x05, 0x09, 0x03, 0x1C, 0x1A, 0x00}},
    {0xF1, 8, {0x36, 0x04, 0x00, 0x3C, 0x0F, 0x0F, 0xA4, 0x02}},
    {0xF2, 9, {0x18, 0xA3, 0x12, 0x02, 0x32, 0x12, 0xFF, 0x32, 0x00}},
    {0xF4, 5, {0x40, 0x00, 0x08, 0x91, 0x04}},
    {0xF8, 2, {0x21, 0x04}},
    {0x3A, 1, {0x55}},                         // 16 bits/pixel (RGB565)
}};
} // namespace

void Ili9486Display::claim_bus() { spi_set_baudrate(m_spi, kCommandBaud); }

void Ili9486Display::write_command(uint8_t cmd) {
    // Command: D/C low, exactly ONE byte, CS pulsed around it -- the CS edge
    // is what latches the shift register's word into the controller, so the
    // per-item toggle is load-bearing, not stylistic (see header).
    gpio_put(kPinDc, 0);
    gpio_put(kPinCs, 0);
    spi_write_blocking(m_spi, &cmd, 1);
    gpio_put(kPinCs, 1);
}

void Ili9486Display::write_data_byte(uint8_t data) {
    // Data: D/C high, sent as SIXTEEN bits (0x00 pad first), because the
    // controller sits behind the board's shift register on a 16-bit-wide
    // register interface. An 8-bit value (what a direct ILI9486 takes)
    // leaves the controller uninitialised and the panel blank.
    const uint8_t buf[2] = {0x00, data};
    gpio_put(kPinDc, 1);
    gpio_put(kPinCs, 0);
    spi_write_blocking(m_spi, buf, 2);
    gpio_put(kPinCs, 1);
}

void Ili9486Display::init() {
    // GPIO10/11/12 belong to SPI1 on this board. MISO is muxed for a future
    // Xpt2046Touch (Phase 3), which shares this bus; the LCD itself never
    // reads.
    spi_init(m_spi, kCommandBaud);
    gpio_set_function(kPinSck, GPIO_FUNC_SPI);
    gpio_set_function(kPinMosi, GPIO_FUNC_SPI);
    gpio_set_function(kPinMiso, GPIO_FUNC_SPI);

    gpio_init(kPinCs);
    gpio_set_dir(kPinCs, GPIO_OUT);
    gpio_put(kPinCs, 1);

    gpio_init(kPinDc);
    gpio_set_dir(kPinDc, GPIO_OUT);
    gpio_put(kPinDc, 1);

    gpio_init(kPinRst);
    gpio_set_dir(kPinRst, GPIO_OUT);

    m_dma_chan = dma_claim_unused_channel(true);

    // Hardware reset with the vendor reference's (generous) 500ms timings
    // rather than datasheet minimums -- power rails come up through the same
    // shift-register board, and shorter pulses did not start it during
    // TOM6809's bring-up.
    gpio_put(kPinRst, 1);
    sleep_ms(500);
    gpio_put(kPinRst, 0);
    sleep_ms(500);
    gpio_put(kPinRst, 1);
    sleep_ms(500);

    for (const InitCommand& entry : kInitSequence) {
        write_command(entry.cmd);
        for (uint8_t i = 0; i < entry.len; ++i) {
            write_data_byte(entry.data[i]);
        }
    }

    // Scan direction / orientation: display function control first, then
    // MADCTL.
    write_command(kCmdDisFunCtrl);
    write_data_byte(0x00);
    write_data_byte(kDisFunLandscape);

    write_command(kCmdMadctl);
    write_data_byte(kMadctlLandscape);
    sleep_ms(200);

    write_command(kCmdSleepOut);
    sleep_ms(120);

    write_command(kCmdDisplayOn);
}

void Ili9486Display::set_window(int x0, int y0, int x1, int y1) {
    // Opens a whole frame's worth of traffic (this call plus the
    // write_pixels() run that follows), so this is where the bus rate gets
    // claimed back from any touch read that happened since the last frame.
    claim_bus();

    // Each command/parameter gets its own CS pulse (see write_command()).
    // x1/y1 are inclusive here, hence no -1.
    write_command(kCmdCaset);
    write_data_byte(static_cast<uint8_t>(x0 >> 8));
    write_data_byte(static_cast<uint8_t>(x0 & 0xFF));
    write_data_byte(static_cast<uint8_t>(x1 >> 8));
    write_data_byte(static_cast<uint8_t>(x1 & 0xFF));

    write_command(kCmdPaset);
    write_data_byte(static_cast<uint8_t>(y0 >> 8));
    write_data_byte(static_cast<uint8_t>(y0 & 0xFF));
    write_data_byte(static_cast<uint8_t>(y1 >> 8));
    write_data_byte(static_cast<uint8_t>(y1 & 0xFF));

    write_command(kCmdRamwr);

    // spi_set_baudrate() disables the SPI peripheral (clears SSE), rewrites
    // its clock prescale/postdiv, then re-enables it. Done BEFORE asserting
    // CS: doing it after risks a spurious SCK/MOSI edge while the shift
    // register is latching, corrupting the start of the burst (confirmed on
    // TOM6809's real hardware as the cause of a per-row horizontal pixel
    // shift).
    spi_set_baudrate(m_spi, kPixelBaud);

    // One-shot: spi_set_baudrate() picks the closest achievable divisor of
    // clk_peri, which need not be what was asked for. Measured throughput
    // (~1.8 MB/s) implies well under the requested 24 MHz (~3 MB/s), and
    // switching write_pixels() to DMA made no difference -- pointing at the
    // achieved clock itself, not CPU-side overhead. Logging the real number
    // once confirms or rules that out.
    {
        static bool logged = false;
        if (!logged) {
            printf("Ili9486Display: pixel baud requested %lu, achieved %u\n",
                   static_cast<unsigned long>(kPixelBaud), spi_get_baudrate(m_spi));
            logged = true;
        }
    }

    // Open the pixel run: unlike the single-byte parameters above, pixel
    // data streams as one continuous CS-low burst of 16-bit values, so CS
    // stays asserted from here until end_write().
    gpio_put(kPinDc, 1);
    gpio_put(kPinCs, 0);
}

void Ili9486Display::end_write() { gpio_put(kPinCs, 1); }

void Ili9486Display::write_pixels(std::span<const uint16_t> pixels) {
    // Pixels must already be byte-swapped to big-endian wire order by the
    // caller. Unlike the 8-bit parameters above, these are genuine 16-bit
    // values -- no 0x00 padding. Transfer unit is still 8 bits (DMA_SIZE_8,
    // matching spi_init()'s default data size): the panel wants two
    // sequential 8-bit SPI words per pixel, not one 16-bit word, so this
    // reinterprets the buffer as a flat byte stream, same as the old
    // spi_write_blocking() call did.
    dma_channel_config cfg = dma_channel_get_default_config(m_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, spi_get_dreq(m_spi, true));
    dma_channel_configure(m_dma_chan, &cfg,
                           &spi_get_hw(m_spi)->dr,
                           pixels.data(),
                           pixels.size_bytes(),
                           true /* start immediately */);
    dma_channel_wait_for_finish_blocking(m_dma_chan);
}

void Ili9486Display::fill_solid(uint16_t rgb565) {
    uint16_t wire = static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
    std::array<uint16_t, kWidth> row;
    row.fill(wire);

    set_window(0, 0, kWidth - 1, kHeight - 1);
    for (int y = 0; y < kHeight; ++y) {
        write_pixels(row);
    }
    end_write();
}
