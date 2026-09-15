// Line-oriented runtime-tuning console over the same USB CDC used for
// printf (pico_stdio_usb). Replaces the F1/F2/F3/F4/F10/F11/F12 keyboard
// intercepts that used to live in src/i_input_usbhid.cpp (now freed for
// vanilla DOOM -- help/save/load/volume/quit/gamma-toggle/spy-mode, see
// doom/m_menu.c and doom/g_game.c). Polled once per tic from I_StartTic();
// 35 Hz is plenty for typing speed.
//
// Commands:
//   help                 show available commands
//   gamma                show current gamma factor
//   gamma up|down        +/- 0.1  (clamped [0.1, 10])
//   gamma <v>            set gamma factor
//   pclk                 show SPI pixel clock request/actual (LCD build)
//   pclk up|down         +/- 1 MHz (LCD build)
//   pclk <hz>            set SPI pixel clock (LCD build)
//   mouse                show mouse posting state
//   mouse on|off         enable/disable mouselook posting
//   sens                 show mouse sensitivity (0-9)
//   sens up|down         +/- 1  (clamped [0, 9])
//   sens <n>             set mouse sensitivity
//
// Separate from the engine: DOOM never reads stdin on the Pico (its only
// getchar() is in d_main.c's '-file'/modifiedgame branch, unreachable here
// since myargc=1), so this owns CDC input exclusively.
#include "i_serial_console.hpp"
#include "pico/stdio.h"

extern "C" {
#include "doomstat.h" // mouseSensitivity
}

extern "C" {
// src/i_video_ili9486.cpp / src/i_video_dvi.cpp -- gamma factor.
float i_video_gamma(void);
void i_video_set_gamma(float gamma);
#ifndef PICODOOM_HDMI
// src/i_video_ili9486.cpp -- SPI pixel clock (DVI has no pixel clock).
uint32_t i_video_pixel_clock_hz(void);
void i_video_set_pixel_clock_hz(uint32_t hz);
#endif
// src/i_input_usbhid.cpp -- mouselook posting.
bool i_input_mouse_enabled(void);
void i_input_set_mouse_enabled(bool enabled);
}

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr float kGammaMin = 0.1f;
constexpr float kGammaMax = 10.0f;
constexpr float kGammaStep = 0.1f;
constexpr int kSensMin = 0;
constexpr int kSensMax = 9;
constexpr int32_t kPixelClockStepHz = 1'000'000;
constexpr uint32_t kPixelClockMinHz = 1'000'000;
constexpr size_t kLineMax = 64;

char g_line[kLineMax];
int g_line_len = 0;
bool g_hint_printed = false;

void print_help()
{
#ifndef PICODOOM_HDMI
    printf("PicoDoom: commands: gamma [up|down|<0.1-10>]  "
           "pclk [up|down|<hz>]  "
           "mouse [on|off]  "
           "sens [up|down|<0-9>]  help\n");
#else
    printf("PicoDoom: commands: gamma [up|down|<0.1-10>]  "
           "mouse [on|off]  "
           "sens [up|down|<0-9>]  help\n");
#endif
}

// Turn a null-terminated string into a trimmed token by returning a pointer
// past any leading whitespace and writing a NUL at the next whitespace or
// end of string.  *rest is advanced past the consumed token to the next
// whitespace, or set to NULL when nothing follows.
char* next_token(char** rest)
{
    char* s = *rest;
    while (*s == ' ' || *s == '\t')
        ++s;
    if (*s == '\0') { *rest = s; return nullptr; }
    char* tok = s;
    while (*s != '\0' && *s != ' ' && *s != '\t')
        ++s;
    if (*s != '\0') {
        *s = '\0';
        *rest = s + 1;
    } else {
        *rest = s;
    }
    return tok;
}

void cmd_gamma(char* args)
{
    char* arg = next_token(&args);
    if (!arg) {
        printf("PicoDoom: gamma %.2f\n", i_video_gamma());
        return;
    }
    if (std::strcmp(arg, "up") == 0) {
        i_video_set_gamma(i_video_gamma() + kGammaStep);
        return;
    }
    if (std::strcmp(arg, "down") == 0) {
        i_video_set_gamma(i_video_gamma() - kGammaStep);
        return;
    }
    char* end;
    float v = std::strtof(arg, &end);
    if (end == arg || *end != '\0') {
        printf("PicoDoom: gamma: unknown arg '%s' (use up, down, or <float>)\n", arg);
        return;
    }
    if (v < kGammaMin || v > kGammaMax)
        printf("PicoDoom: gamma: clamped to [%.1f..%.1f]\n", kGammaMin, kGammaMax);
    i_video_set_gamma(v);
}

#ifndef PICODOOM_HDMI
void cmd_pclk(char* args)
{
    char* arg = next_token(&args);
    if (!arg) {
        printf("PicoDoom: SPI pixel clock requested=%u Hz\n",
               (unsigned)i_video_pixel_clock_hz());
        return;
    }
    if (std::strcmp(arg, "up") == 0) {
        i_video_set_pixel_clock_hz(i_video_pixel_clock_hz() + kPixelClockStepHz);
        return;
    }
    if (std::strcmp(arg, "down") == 0) {
        uint32_t cur = i_video_pixel_clock_hz();
        uint32_t next = cur > kPixelClockStepHz ? cur - kPixelClockStepHz : kPixelClockMinHz;
        i_video_set_pixel_clock_hz(next);
        return;
    }
    char* end;
    unsigned long v = std::strtoul(arg, &end, 10);
    if (end == arg || *end != '\0') {
        printf("PicoDoom: pclk: unknown arg '%s' (use up, down, or <hz>)\n", arg);
        return;
    }
    i_video_set_pixel_clock_hz(static_cast<uint32_t>(v));
}
#endif

void cmd_mouse(char* args)
{
    char* arg = next_token(&args);
    if (!arg) {
        printf("PicoDoom: mouse %s\n", i_input_mouse_enabled() ? "on" : "off");
        return;
    }
    if (std::strcmp(arg, "on") == 0) {
        i_input_set_mouse_enabled(true);
        printf("PicoDoom: mouse on\n");
        return;
    }
    if (std::strcmp(arg, "off") == 0) {
        i_input_set_mouse_enabled(false);
        printf("PicoDoom: mouse off\n");
        return;
    }
    printf("PicoDoom: mouse: unknown arg '%s' (use on or off)\n", arg);
}

void cmd_sens(char* args)
{
    char* arg = next_token(&args);
    if (!arg) {
        printf("PicoDoom: mouse sensitivity %d\n", mouseSensitivity);
        return;
    }
    if (std::strcmp(arg, "up") == 0) {
        if (mouseSensitivity < kSensMax)
            mouseSensitivity++;
        printf("PicoDoom: mouse sensitivity %d\n", mouseSensitivity);
        return;
    }
    if (std::strcmp(arg, "down") == 0) {
        if (mouseSensitivity > kSensMin)
            mouseSensitivity--;
        printf("PicoDoom: mouse sensitivity %d\n", mouseSensitivity);
        return;
    }
    char* end;
    long v = std::strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || v < kSensMin || v > kSensMax) {
        printf("PicoDoom: sens: expected integer [%d..%d]\n", kSensMin, kSensMax);
        return;
    }
    mouseSensitivity = static_cast<int>(v);
    printf("PicoDoom: mouse sensitivity %d\n", mouseSensitivity);
}

void process_line(char* line, int len)
{
    line[len] = '\0';
    char* rest = line;
    char* cmd = next_token(&rest);
    if (!cmd)
        return;

    if (std::strcmp(cmd, "help") == 0 || std::strcmp(cmd, "?") == 0) {
        print_help();
        return;
    }
    if (std::strcmp(cmd, "gamma") == 0) { cmd_gamma(rest); return; }
#ifndef PICODOOM_HDMI
    if (std::strcmp(cmd, "pclk") == 0) { cmd_pclk(rest); return; }
#endif
    if (std::strcmp(cmd, "mouse") == 0) { cmd_mouse(rest); return; }
    if (std::strcmp(cmd, "sens") == 0) { cmd_sens(rest); return; }
    printf("PicoDoom: unknown command '%s' -- type 'help' for a list\n", cmd);
}

} // namespace

void i_serial_console_poll()
{
    if (!g_hint_printed) {
        g_hint_printed = true;
        printf("PicoDoom: serial console ready -- type 'help' for commands\n");
    }
    for (;;) {
        int c = stdio_getchar_timeout_us(0);
        if (c < 0) // PICO_ERROR_TIMEOUT or EOF
            break;
        if (c == '\r' || c == '\n') {
            if (g_line_len > 0) {
                process_line(g_line, g_line_len);
                g_line_len = 0;
            }
        } else if (c == 0x08 || c == 0x7F) { // backspace / DEL
            if (g_line_len > 0)
                --g_line_len;
        } else if (c >= 0x20 && c <= 0x7E) {
            if (g_line_len < static_cast<int>(kLineMax - 1))
                g_line[g_line_len++] = static_cast<char>(c);
        }
    }
}
