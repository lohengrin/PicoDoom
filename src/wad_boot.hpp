#pragma once
// Boot WAD-selection glue shared between the LVGL menu (src/wad_menu.cpp)
// and the engine (doom/d_main.c, doom/m_misc.c -- Phases 4/4a). All C
// linkage: the engine side declares these by hand under #ifdef PICO rather
// than including this header (doom/ stays upstream-close), the menu side
// calls them directly.

#ifdef __cplusplus
extern "C" {
#endif

// The WAD the boot menu chose, or NULL when the menu was skipped/cancelled.
// Owned by wad_boot.cpp forever (boot-lifetime static buffer).
const char* pico_selected_wad(void);

// Store the menu's choice (empty string NULL-safe: treated as "cancel").
void pico_wad_select(const char* name);

// GameMode_t for `name` by sniffing its lump directory (MAP01 -> commercial,
// else E1M1 with E4M1->retail / E3M1->registered / none->shareware; no
// markers -> commercial). The engine's IdentifyVersion() calls this with the
// selection before the fixed-name scan would run.
int pico_wad_gamemode(const char* name);

// 1 if `name` looks like a Doom-family IWAD (Doom/Doom II/Ultimate/Freedoom/
// TNT/Plutonia), 0 if it looks like a Raven IWAD (Heretic/Hexen, detected via
// the TINTTAB lump) that this Doom-only engine has no game logic for. Callers
// should refuse to load a 0 rather than let it crash deep in the renderer.
int pico_wad_is_doom_family(const char* name);

// Last menu selection persisted in default.cfg ("picodoom_lastwad"), or ""
// when absent. Read once, cached.
const char* pico_wad_last(void);

// Rewrite default.cfg so the picodoom_lastwad key holds `name` (replacing an
// existing line, appended otherwise). Called by the menu at select time so
// the engine's M_LoadDefaults() picks it up for the auto-highlight on next
// boot; M_SaveDefaults() re-saves it in full on clean quit.
void pico_wad_save_last(const char* name);

// LCD "high res" (480x300) setting persisted in default.cfg as
// "picodoom_hires" (0/1). Build default when absent: 1 on ST7796, 0 on ILI9486.
int pico_hires_setting(void);
void pico_hires_save(int hires);

// Mutable target of doom/m_misc.c's "picodoom_lastwad" defaults[] entry
// (declared there as `extern char* pico_last_wad;` under #ifdef PICO).
extern char* pico_last_wad;
extern int pico_hires; // mirror of the "picodoom_hires" defaults[] entry

#ifdef __cplusplus
}
#endif