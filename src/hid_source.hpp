#pragma once

// PicoDoom's input seam: the transport-neutral HidSource now lives in Pico-Toolset
// (pico_toolset/hid_source.h, with UsbHidSource / BluetoothHidSource adapters). I_StartTic()
// (src/i_input_tic.cpp) and the boot menu's LVGL indevs (src/lvgl_indev.cpp) read the active
// backend through it; each build supplies exactly one: src/i_input_usbhid.cpp (PiZero) or
// src/i_input_presto.cpp (Bluetooth HID + QwSTPad).
#include "pico_toolset/hid_source.h"

using pico_toolset::HidMouse;
using pico_toolset::HidSource;

// Provided by the input backend selected at build time.
HidSource& i_input_hid(void);
// Per-poll service hook: called first thing in I_StartTic() and from the boot menu's loop (USB on
// core0 on HDMI; I2C pad reads and Bluetooth mouse folding on Presto; no-op otherwise).
void i_input_backend_poll(void);
