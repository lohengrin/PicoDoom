#pragma once

// What the Bluetooth page (src/menu_board_presto.cpp) needs from the Presto's radio code.
#include "pico_toolset/bluetooth_hid.h"

#ifdef PICODOOM_A2DP
#include "pico_toolset/bluetooth_a2dp.h"
// The headset component, or null if it did not come up (src/presto_a2dp.cpp).
pico_toolset::BluetoothA2dp* presto_a2dp_device();
#endif

// The radio and HID host came up (src/i_input_presto.cpp).
bool presto_bt_available(void);
pico_toolset::BluetoothHid& presto_bt_hid(void);
