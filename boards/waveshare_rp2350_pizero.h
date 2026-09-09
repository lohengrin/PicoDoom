/*
 * Copyright (c) 2024 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// This header may be included by other board headers as "boards/waveshare_rp2350_pizero.h"

#ifndef _BOARDS_WAVESHARE_RP2350_PIZERO
#define _BOARDS_WAVESHARE_RP2350_PIZERO

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define WAVESHARE_RP2350_PIZERO

// --- RP2350 VARIANT ---
#define PICO_RP2350A 0

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// The PRO Micro doesn't have a plain LED, but a WS2812
#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 2
#endif

// --- I2C ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (4 * 1024 * 1024)
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// pico_cmake_set_default PICO_RP2350_A2_SUPPORTED = 1
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// --- USB-PIO host (Phase 3, WAVESHARE_USB_HID_SUPPORT) ---
// D+/D- pins for the board's integrated USB-PIO port, per Waveshare's own
// RP2350-PiZero wiki. Pico-PIO-USB assumes D- = D+ + 1 (GPIO29), matching
// this pinout. Defined unconditionally (harmless when
// WAVESHARE_USB_HID_SUPPORT is OFF) -- same convention as every other pin
// default in this header. See README-PICO.md's GPIO table: GPIO28/29 are
// free on this board (uSD uses GPIO30/31/40/43, LCD/touch uses
// GPIO7/8/10/11/12/17/24/25).
#ifndef PICO_DEFAULT_PIO_USB_DP_PIN
#define PICO_DEFAULT_PIO_USB_DP_PIN 28
#endif

// --- PSRAM (Phase 4, WAVESHARE_PSRAM_SUPPORT) ---
// Chip select for the 8MB QSPI PSRAM chip wired to this board's GPIO47.
// Defined unconditionally (harmless when WAVESHARE_PSRAM_SUPPORT is OFF,
// same convention as every other pin default in this header) -- actual use
// is gated by include/pico/Psram.hpp's psram_hw_init(), which is only
// compiled in when that option is on.
#ifndef PICO_PSRAM_CS_PIN
#define PICO_PSRAM_CS_PIN 47
#endif

#endif