// Serial console for Pico-specific runtime tuning (src/i_serial_console.cpp):
// the commands that used to live on F1/F2/F3/F4/F10/F11/F12 keyboard
// intercepts in src/i_input_usbhid.cpp before those keys were freed for
// vanilla DOOM use. Polled once per tic from I_StartTic(), both builds.
#ifndef I_SERIAL_CONSOLE_HPP
#define I_SERIAL_CONSOLE_HPP

extern "C" void i_serial_console_poll(void);

#endif