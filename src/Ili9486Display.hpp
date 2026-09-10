#pragma once
#include "hardware/spi.h"
#include <cstdint>
#include <span>

/**
 * Low-level driver for the Waveshare 3.5" RPi LCD (A)'s ILI9486 controller,
 * over the Waveshare RP2350-PiZero's real hardware SPI1 (see AGENTS.md's
 * GPIO table): SCK=GPIO10, MOSI=GPIO11, MISO=GPIO12 (touch controller only,
 * see below), CS=GPIO8, D/C(RS)=GPIO24, RST=GPIO25.
 *
 * Ported verbatim from /home/lohengrin/Dev/TOM6809's validated
 * include/pico/Ili9486Display.hpp + src/ui_pico/Ili9486Display.cpp (same
 * exact board + panel) -- see that project for the full bring-up history.
 *
 * WIRE PROTOCOL -- this panel is NOT a directly-wired ILI9486: the controller
 * sits behind a shift register on the LCD board, which makes three things
 * mandatory (transcribed from Waveshare's own reference driver; Linux encodes
 * the same panel as fbtft's `waveshare35a` profile with `regwidth=16`):
 *   1. A command is ONE byte with D/C low.
 *   2. A register parameter is SIXTEEN bits with D/C high -- a 0x00 pad byte
 *      followed by the value. A bare 8-bit parameter (what a generic ILI9486
 *      takes) leaves the controller uninitialised: the panel stays blank
 *      forever, with no error anywhere.
 *   3. CS is pulsed around EVERY individual command and parameter -- the CS
 *      edge is what latches each word out of the shift register. Holding CS
 *      low across a command plus parameters (the usual SPI-display idiom)
 *      does not work here. The one exception is pixel data, which streams as
 *      one continuous CS-low burst (see set_window()/end_write() below).
 * Pixel data itself is genuine 16-bit RGB565, big-endian on the wire, with no
 * padding -- only the 8-bit register parameters get the 0x00 pad.
 *
 * The panel is also WRITE-ONLY here: the board's pin 21 (MISO) carries
 * "SPI data output of touch panel" per its own pinout -- wired to the
 * XPT2046 only, never the ILI9486. Register reads can never return anything
 * on this hardware.
 */
class Ili9486Display {
public:
    static constexpr int kWidth = 480;
    static constexpr int kHeight = 320;

    /// Resets the panel and runs the ILI9486 init register sequence (power
    /// control, VCOM, MADCTL orientation/RGB-order, RGB565, sleep-out,
    /// display-on).
    void init();

    /// Sets the RAMWR addressing window (CASET x0..x1, PASET y0..y1,
    /// inclusive), issues RAMWR (0x2C), and leaves CS asserted -- every
    /// subsequent write_pixels() streams into this window. Callers MUST call
    /// end_write() once done; no touch read may happen between set_window()
    /// and end_write().
    void set_window(int x0, int y0, int x1, int y1);

    /// Streams `pixels.size()` RGB565 pixels (already byte-swapped to
    /// big-endian wire order) into the window set by the most recent
    /// set_window(). Callers must not write more pixels than the window
    /// holds (no bounds tracking).
    void write_pixels(std::span<const uint16_t> pixels);

    /// Deasserts CS after a set_window()/write_pixels() sequence, freeing the
    /// shared SPI1 bus for a touch read. Must be called exactly once after
    /// the last write_pixels() for a given set_window().
    void end_write();

    /// Fills the whole panel with one solid color -- bring-up test pattern.
    void fill_solid(uint16_t rgb565);

    [[nodiscard]] spi_inst_t* spi() const { return m_spi; }

private:
    /// Re-asserts this display's SPI baud rate. A shared touch read (Phase 3)
    /// may drop the bus rate without restoring it, so every LCD transaction
    /// must claim the bus rate back first.
    void claim_bus();
    void write_command(uint8_t cmd);
    void write_data_byte(uint8_t data);

    spi_inst_t* m_spi = spi1;
    // write_pixels() streams over this DMA channel instead of the SDK's
    // spi_write_blocking() -- that function is already TX-only, but still
    // polls the TX-FIFO-not-full flag one byte at a time from the CPU,
    // which both costs cycles and is vulnerable to USB's periodic IRQ
    // stealing time mid-burst. DMA paces itself off the SPI's TX DREQ
    // entirely in hardware, immune to either. Claimed once in init().
    int m_dma_chan = -1;
};
