#pragma once

// TinyUSB config for Phase 3's USB-PIO HID keyboard host (see
// src/PicoUsbKeyboard.cpp). Ported from TOM6809's validated same-board
// tusb_config.h (WAVESHARE_USB_HID_SUPPORT) rather than re-derived.
//
// IMPORTANT: this project already links pico_stdio_usb (pico_enable_stdio_usb()
// in CMakeLists.txt) for the serial console, which itself ships a default
// tusb_config.h (pico-sdk/src/rp2_common/pico_stdio_usb/include/tusb_config.h)
// providing CFG_TUD_CDC=1 / CFG_TUSB_RHPORT0_MODE=(OPT_MODE_DEVICE) etc. --
// but that file's content is guarded behind
// `#if !defined(LIB_TINYUSB_HOST) && !defined(LIB_TINYUSB_DEVICE)`, which
// becomes FALSE (i.e. that file goes empty) the moment anything links
// tinyusb_host, as this project now does for the keyboard. This file is
// therefore NOT additive to that one -- it fully REPLACES it, and must
// re-declare every device-side setting the serial console needs (CFG_TUD_CDC
// and friends below) in addition to the new host-side ones, or stdio_usb
// silently stops working. CMakeLists.txt's target_include_directories()
// already puts src/ ahead of pico_stdio_usb's own include dir, so this file
// is the one the build actually picks up.
//
// Native USB peripheral (rhport 0) stays the *device* role -- exactly what
// stdio_usb already uses it for -- while the keyboard host stack runs
// entirely over Pico-PIO-USB's PIO-emulated root hub (rhport 1, see
// PicoUsbKeyboard::host_stack_setup()'s tuh_configure()/tuh_init() calls,
// pinned to PICO_DEFAULT_PIO_USB_DP_PIN = GPIO28,
// boards/waveshare_rp2350_pizero.h). Two independent controllers (one
// native, one PIO-emulated) coexisting in one TinyUSB build, not a conflict.

#define CFG_TUSB_MCU              OPT_MCU_RP2040  // RP2350 uses the same TinyUSB MCU port as RP2040
#define CFG_TUSB_OS               OPT_OS_PICO

// --- Device side (rhport 0, native USB peripheral) -- required to keep the
// serial console (pico_stdio_usb) working now that this file replaces its
// own default tusb_config.h (see doc comment above). Values copied from
// that file's own defaults.
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE)
#define CFG_TUD_ENABLED           1
#define CFG_TUD_CDC               1
#define CFG_TUD_CDC_RX_BUFSIZE    64
#define CFG_TUD_CDC_TX_BUFSIZE    64
#define CFG_TUD_CDC_EP_BUFSIZE    64
#define CFG_TUD_VENDOR            0

// --- Host side (rhport 1, Pico-PIO-USB) -- the new part.
#define CFG_TUH_RPI_PIO_USB       1
#define CFG_TUH_ENABLED           1
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB               1
#define CFG_TUH_DEVICE_MAX        (CFG_TUH_HUB ? 4 : 1) // a hub typically has 4 ports

// One keyboard interface, with a little headroom for a composite device
// exposing more than one (e.g. a keyboard's separate consumer-control/
// media-key collection).
#define CFG_TUH_HID               4
#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64
