// USB HID keyboard/mouse/gamepad backend (Pico-Toolset's pico_toolset_usb_hid,
// PIO-USB TinyUSB host) for the Waveshare RP2350-PiZero builds. The engine-facing
// event posting lives in src/i_input_tic.cpp; this file owns the host stack and
// exposes it as a HidSource.
//
// LCD build (default, !PICODOOM_HDMI): also owns core1 -- this stack's
// SOF-timer IRQ binds to whichever core calls tuh_init(), so init() happens
// from inside core1_entry() (via UsbHidHost::init() with run_on_core1=false
// -- kWaveshareRp2350PiZeroLcdManualCore1 -- rather than letting init()
// spawn and own core1 itself), letting this same core1 loop also drive
// i_video_core1_step() (the LCD chunked blit, src/i_video_spi_lcd.cpp)
// interleaved with tuh_task() -- see i_video_core1.hpp for why that
// interleaving (not one blocking per-frame blit call) matters: Pico-PIO-USB's
// software-timed bus servicing would otherwise starve for the ~41ms a full
// frame's SPI feed takes.
//
// HDMI build (PICODOOM_HDMI): core1 belongs exclusively to
// src/i_video_dvi.cpp's DVI encode loop -- libdvi's per-scanline DMA IRQ
// has zero scheduling slack and cannot share a core with Pico-PIO-USB's
// 1ms SOF-timer IRQ (confirmed on real hardware in TOM6809: no picture at
// all when shared). So the USB host stack runs on core0 instead, using the
// toolset's kWaveshareRp2350PiZeroHdmi preset (run_on_core1=false, its own PIO
// instance) -- init() happens once from src/PicoDoom.cpp's main() and
// UsbHidHost::task() is polled once per tic via i_input_backend_poll()
// (looser than TinyUSB's native polling, a cost this build accepts since
// there's no SPI-feed core1 contention to avoid instead).
#include "hid_source.hpp"
#include "pico_toolset/usb_hid_host.h"
#include "pico_toolset/usb_hid_configs.h"
#ifdef PICODOOM_SPI_LCD
#include "i_video_core1.hpp"
#endif

#include "pico/multicore.h"

#include <cstdint>

namespace {

pico_toolset::UsbHidHost g_usb_hid;

class UsbHidSource final : public HidSource {
public:
    bool is_key_down(uint8_t usage) const override { return g_usb_hid.is_key_down(usage); }
    bool is_modifier_down(uint8_t mask) const override { return g_usb_hid.is_modifier_down(mask); }
    HidMouse mouse_state() const override
    {
        pico_toolset::UsbHidHost::MouseState m = g_usb_hid.mouse_state();
        HidMouse out;
        out.present = m.present;
        out.x = m.x;
        out.y = m.y;
        out.left = m.left_button;
        out.right = m.right_button;
        out.middle = m.middle_button;
        return out;
    }
    void consume_mouse_delta(int& dx, int& dy) override { g_usb_hid.consume_mouse_delta(dx, dy); }
    int consume_mouse_wheel() override { return g_usb_hid.consume_mouse_wheel(); }
    pico_toolset::GamepadState gamepad_state(size_t index) const override { return g_usb_hid.gamepad_state(index); }
    uint8_t connected_keyboard_count() const override { return g_usb_hid.connected_keyboard_count(); }
};

UsbHidSource g_source;

#ifdef PICODOOM_SPI_LCD
// core1's stack -- deliberately not the default multicore_launch_core1()
// mechanism, which reserves PICO_CORE1_STACK_SIZE out of the tiny, fixed
// SCRATCH_X SRAM bank (a couple KB at most -- too small for the TinyUSB host
// stack + Pico-PIO-USB's own bit-banging call chains, per TOM6809's
// real-hardware testing on this same board/library combination). 16KB
// carved out of ordinary SRAM instead, matching TOM6809's validated size.
constexpr size_t kCore1StackWords = 4096; // 16KB
uint32_t g_core1_stack[kCore1StackWords];

void core1_entry()
{
    // run_on_core1=false: init() only brings up the host stack (tuh_init())
    // on whichever core calls it -- here, core1 -- and returns immediately.
    // This loop then drives both task() and the LCD blit stepper, so a pending
    // frame's SPI feed can't starve USB servicing.
    g_usb_hid.init(pico_toolset::configs::usb_hid::kWaveshareRp2350PiZeroLcdManualCore1);
    while (true) {
        pico_toolset::UsbHidHost::task();
        i_video_core1_step();
    }
}
#endif // PICODOOM_SPI_LCD

} // namespace

#ifdef PICODOOM_SPI_LCD
// Launches core1 (see core1_entry() above). Called once from main()
// (src/PicoDoom.cpp), before D_DoomMain() -- gives the keyboard time to
// enumerate while the WAD loads and the engine initializes.
extern "C" void usb_hid_core1_init(void)
{
    multicore_reset_core1(); // defensive, matches Pico-PIO-USB's own reference examples
    multicore_launch_core1_with_stack(core1_entry, g_core1_stack, sizeof(g_core1_stack));
}
#else
// HDMI build's counterpart: brings up the host stack on core0 (see file
// header) instead of launching core1 -- core1 belongs to src/i_video_dvi.cpp.
extern "C" void usb_hid_core0_init(void)
{
    g_usb_hid.init(pico_toolset::configs::usb_hid::kWaveshareRp2350PiZeroHdmi);
}
#endif

HidSource& i_input_hid(void)
{
    return g_source;
}

// LCD build: no-op -- core1 (core1_entry above) already calls
// UsbHidHost::task() every loop iteration, so core0 must not double-poll.
// HDMI build: core0 owns the stack; polled once per tic and from the boot menu.
void i_input_backend_poll(void)
{
#ifdef PICODOOM_HDMI
    pico_toolset::UsbHidHost::task();
#endif
}
