// Presto ambient LEDs (7 WS2812 behind the panel) as a health display:
//   base colour  green (>=100%) -> orange (50%) -> red (0%), linear in between
//   health down  red flash,   health up  green flash,
//   any other pickup (ammo, armour, weapon, key, power-up)  blue flash.
// No engine edits: i_leds_update() diffs a snapshot of the console player's state each
// poll (called from i_input_backend_poll(): every tic in game, every menu pass otherwise).
// Dark outside a level. Flashes fade linearly over kFlashMs into the base colour.
#include "led_colours.hpp"
#include "pico_toolset/ws2812.h"
#include "pico_toolset/ws2812_configs.h"

extern "C" {
#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
}

#include "pico/time.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t kFlashMs = 450;
constexpr float kBaseBrightness = 0.25f; // ambient level; flashes go to full

using picodoom::Flash;
using picodoom::Rgb;

pico_toolset::Ws2812 g_leds;
bool g_ok = false;

bool g_valid = false;
gamestate_t g_prev_state = GS_DEMOSCREEN;
int g_health = 0, g_armor = 0;
int g_ammo[NUMAMMO];
bool g_weapons[NUMWEAPONS];
bool g_cards[NUMCARDS];
int g_powers[NUMPOWERS];

Flash g_flash = Flash::None;
uint32_t g_flash_start_ms = 0;
uint32_t g_last_rgb = 0xFFFFFFFFu; // last frame pushed, to skip identical writes

void snapshot(const player_t& p) {
    g_health = p.health;
    g_armor = p.armorpoints;
    for (int i = 0; i < NUMAMMO; ++i) g_ammo[i] = p.ammo[i];
    for (int i = 0; i < NUMWEAPONS; ++i) g_weapons[i] = p.weaponowned[i];
    for (int i = 0; i < NUMCARDS; ++i) g_cards[i] = p.cards[i];
    for (int i = 0; i < NUMPOWERS; ++i) g_powers[i] = p.powers[i];
}

// True when anything other than health was gained since the snapshot.
bool gained_item(const player_t& p) {
    if (p.armorpoints > g_armor) return true;
    for (int i = 0; i < NUMAMMO; ++i)
        if (p.ammo[i] > g_ammo[i]) return true;
    for (int i = 0; i < NUMWEAPONS; ++i)
        if (p.weaponowned[i] && !g_weapons[i]) return true;
    for (int i = 0; i < NUMCARDS; ++i)
        if (p.cards[i] && !g_cards[i]) return true;
    for (int i = 0; i < NUMPOWERS; ++i)
        if (p.powers[i] > g_powers[i]) return true;
    return false;
}

void show(const Rgb& c) {
    const uint8_t r = static_cast<uint8_t>(std::clamp(c.r, 0.0f, 255.0f));
    const uint8_t g = static_cast<uint8_t>(std::clamp(c.g, 0.0f, 255.0f));
    const uint8_t b = static_cast<uint8_t>(std::clamp(c.b, 0.0f, 255.0f));
    const uint32_t packed = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    if (packed == g_last_rgb)
        return;
    g_last_rgb = packed;
    for (size_t i = 0; i < g_leds.count(); ++i)
        g_leds.set_rgb(i, r, g, b);
    g_leds.show();
}

} // namespace

// Must run before cyw43_arch_init(): the WS2812 program moves PIO0's GPIO window to
// 16-47 (gpio_base 16), which the radio's PIO SPI then adapts to (toolset Presto guide).
void i_leds_init(void) {
    g_ok = g_leds.init(pico_toolset::configs::ws2812::kPimoroniPresto);
    printf("PicoDoom: LEDs %s\n", g_ok ? "ready" : "init failed");
    if (g_ok)
        show({0, 0, 0});
}

void i_leds_update(void) {
    if (!g_ok)
        return;

    if (gamestate != GS_LEVEL) {
        g_valid = false;
        g_flash = Flash::None;
        g_prev_state = gamestate;
        show({0, 0, 0});
        return;
    }

    const player_t& p = players[consoleplayer];
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (!g_valid || g_prev_state != GS_LEVEL) {
        snapshot(p);
        g_valid = true;
        g_prev_state = GS_LEVEL;
    } else {
        Flash f = Flash::None;
        if (p.health < g_health) f = Flash::Red;
        else if (p.health > g_health) f = Flash::Green;
        else if (gained_item(p)) f = Flash::Blue;
        if (f != Flash::None) {
            g_flash = f;
            g_flash_start_ms = now_ms;
        }
        snapshot(p);
    }

    Rgb c = picodoom::health_colour(p.health);
    c = {c.r * kBaseBrightness, c.g * kBaseBrightness, c.b * kBaseBrightness};
    if (g_flash != Flash::None) {
        const uint32_t age = now_ms - g_flash_start_ms;
        if (age >= kFlashMs)
            g_flash = Flash::None;
        else
            c = picodoom::apply_flash(c, g_flash, age, kFlashMs);
    }
    show(c);
}
