// Boot WAD-selection backend (Phase 4): owns the menu's choice, sniffs an
// IWAD's lumps to pick DOOM's GameMode_t, and persists the last choice in
// default.cfg for the engine's own defaults machinery to pick up.
//
// C++ file with a C-linkage interface (wad_boot.hpp) -- the engine side
// (doom/d_main.c, doom/m_misc.c) declares what it needs under #ifdef PICO,
// this side gives it extern "C" definitions.
#include "wad_boot.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "doomdef.h" // GameMode_t (shareware/registered/commercial/retail)
}

namespace {

// The menu's one-time boot selection. Fixed-size: WAD filenames on FAT are
// 8.3 (max 12 chars), so 16 is plenty. g_selected_set distinguishes "not
// chosen yet" from "chosen but set to empty" (both mean fall-through).
char g_selected[16];
bool g_selected_set = false;

// Last-selection cache, read from default.cfg once on first use.
char g_last[16];
bool g_last_loaded = false;

constexpr const char* kCfgKey = "picodoom_lastwad";
constexpr const char* kHiresKey = "picodoom_hires";
// 480x300 by default on the ST7796 panel (faster SPI, bigger screen);
// ILI9486 keeps the original 320x200.
#ifdef PICODOOM_ST7796
constexpr int kHiresDefault = 1;
#else
constexpr int kHiresDefault = 0;
#endif

// A WAD directory's 8-byte lump-name field is padded with NULs (id's own
// tools) or spaces (some third-party WADs); FAT-side we can't tell which
// without checking both, so match the name then accept either pad.
bool lump_name_is(const char* name8, const char* want, size_t want_len)
{
    if (memcmp(name8, want, want_len) != 0)
        return false;
    return name8[want_len] == '\0' || name8[want_len] == ' ';
}

// Sniff an IWAD's lump directory (skipping the whole-file lumpinfo the
// engine builds later) for the marker lumps that pin its GameMode_t:
//   MAP01          -> commercial (Doom II)
//   E4M1           -> retail     (Ultimate Doom)
//   E3M1           -> registered (Doom 1 registered / The Ultimate Doom*
//                    still carries E1M1+E4M1 so E4M1 wins over E3M1 below)
//   E1M1 only      -> shareware  (Doom shareware)
//   no markers     -> commercial (best guess; e.g. a PWAD-like bare IWAD)
// Directory read in bounded batches (Freedoom2 has ~19k lumps -- a whole-dir
// read would malloc ~304KB, far too much SRAM) into a small stack buffer.
// Returns a doomdef.h GameMode_t value.
int sniff_gamemode(const char* name)
{
    FILE* f = fopen(name, "rb");
    if (!f)
        return commercial;
    fclose(f);
    f = fopen(name, "rb");
    if (!f)
        return commercial;

    uint8_t magic[4];
    uint8_t counts[8];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "IWAD", 4) != 0 ||
        fread(counts, 1, 8, f) != 8) {
        fclose(f);
        return commercial;
    }
    const uint32_t numlumps =
        static_cast<uint32_t>(counts[0]) | static_cast<uint32_t>(counts[1]) << 8 |
        static_cast<uint32_t>(counts[2]) << 16 | static_cast<uint32_t>(counts[3]) << 24;
    const uint32_t dirpos =
        static_cast<uint32_t>(counts[4]) | static_cast<uint32_t>(counts[5]) << 8 |
        static_cast<uint32_t>(counts[6]) << 16 | static_cast<uint32_t>(counts[7]) << 24;
    // Malformed-header guard: don't seek/read forever on a truncated file.
    // Real IWADs top out well under this; the engine's W_AddFile would catch
    // it anyway later, this just bounds the boot-time sniff.
    if (numlumps > 1u << 20) {
        fclose(f);
        return commercial;
    }

    bool map01 = false, e1m1 = false, e3m1 = false, e4m1 = false;
    constexpr uint32_t kBatch = 256;
    uint8_t entry[16];
    for (uint32_t base = 0; base < numlumps;) {
        const uint32_t n = (numlumps - base) < kBatch ? (numlumps - base) : kBatch;
        if (fseek(f, static_cast<long>(dirpos) + static_cast<long>(base) * 16L, SEEK_SET) != 0)
            break;
        for (uint32_t i = 0; i < n && fread(entry, 1, 16, f) == 16; ++i) {
            const char* en = reinterpret_cast<const char*>(entry + 8);
            if (!map01 && lump_name_is(en, "MAP01", 5)) {
                map01 = true;
                break; // commercial decided; no point scanning the rest
            }
            if (!e1m1 && lump_name_is(en, "E1M1", 4))
                e1m1 = true;
            else if (!e3m1 && lump_name_is(en, "E3M1", 4))
                e3m1 = true;
            else if (!e4m1 && lump_name_is(en, "E4M1", 4))
                e4m1 = true;
        }
        if (map01)
            break;
        base += n;
    }
    fclose(f);

    if (map01)
        return commercial;
    if (e4m1)
        return retail;
    if (e3m1)
        return registered;
    if (e1m1)
        return shareware;
    return commercial;
}

// This engine (doom/) is a stock Doom-only linuxdoom port: no Heretic/Hexen
// game logic, switch tables, or renderer support exist anywhere in the tree.
// sniff_gamemode() only tells Doom episodes apart from each other -- it
// can't tell a Doom-family IWAD from a Raven one, since Hexen's MAP01..
// naming collides with Doom II's and Heretic's ExMy naming collides with
// Doom 1's. Both would otherwise sail through IdentifyVersion() and crash
// deep in P_InitSwitchList() (R_TextureNumForName: SW1BRCOM not found) --
// confusing, and far from where the real problem (unsupported IWAD) is.
//
// TINTTAB is a translucency lookup table lump Raven added for Heretic and
// Hexen; no Doom-family IWAD (Doom, Doom II, Ultimate Doom, Freedoom, TNT,
// Plutonia) carries it. Its presence is a reliable "not a Doom IWAD" flag.
bool sniff_is_doom_family(const char* name)
{
    FILE* f = fopen(name, "rb");
    if (!f)
        return true; // let the caller's own access()/fopen() report the real error

    uint8_t magic[4];
    uint8_t counts[8];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "IWAD", 4) != 0 ||
        fread(counts, 1, 8, f) != 8) {
        fclose(f);
        return true;
    }
    const uint32_t numlumps =
        static_cast<uint32_t>(counts[0]) | static_cast<uint32_t>(counts[1]) << 8 |
        static_cast<uint32_t>(counts[2]) << 16 | static_cast<uint32_t>(counts[3]) << 24;
    const uint32_t dirpos =
        static_cast<uint32_t>(counts[4]) | static_cast<uint32_t>(counts[5]) << 8 |
        static_cast<uint32_t>(counts[6]) << 16 | static_cast<uint32_t>(counts[7]) << 24;
    if (numlumps > 1u << 20) {
        fclose(f);
        return true;
    }

    bool tinttab = false;
    constexpr uint32_t kBatch = 256;
    uint8_t entry[16];
    for (uint32_t base = 0; base < numlumps && !tinttab;) {
        const uint32_t n = (numlumps - base) < kBatch ? (numlumps - base) : kBatch;
        if (fseek(f, static_cast<long>(dirpos) + static_cast<long>(base) * 16L, SEEK_SET) != 0)
            break;
        for (uint32_t i = 0; i < n && fread(entry, 1, 16, f) == 16; ++i) {
            const char* en = reinterpret_cast<const char*>(entry + 8);
            if (lump_name_is(en, "TINTTAB", 7)) {
                tinttab = true;
                break;
            }
        }
        base += n;
    }
    fclose(f);
    return !tinttab;
}

void read_last_from_cfg()
{
    g_last[0] = '\0';
    FILE* f = fopen("default.cfg", "r");
    if (!f)
        return;
    char key[80];
    char val[100];
    while (fscanf(f, "%79s %[^\n]\n", key, val) == 2) {
        if (strcmp(key, kCfgKey) == 0) {
            // Same layout M_LoadDefaults() writes: `key\t\t"value"`
            const size_t len = strlen(val);
            if (len >= 3 && val[0] == '"' && val[len - 1] == '"') {
                const size_t clen = len - 2;
                const size_t n = clen < sizeof(g_last) - 1 ? clen : sizeof(g_last) - 1;
                memcpy(g_last, val + 1, n);
                g_last[n] = '\0';
            }
            break;
        }
    }
    fclose(f);
}

} // namespace

extern "C" char* pico_last_wad = nullptr;
extern "C" int pico_hires = 0;

extern "C" const char* pico_selected_wad(void)
{
    return g_selected_set ? g_selected : nullptr;
}

extern "C" void pico_wad_select(const char* name)
{
    if (name == nullptr) {
        g_selected_set = false;
        g_selected[0] = '\0';
        return;
    }
    const size_t n = strlen(name);
    g_selected_set = n > 0;
    const size_t cap = sizeof(g_selected) - 1;
    memcpy(g_selected, name, n < cap ? n : cap);
    g_selected[n < cap ? n : cap] = '\0';
}

extern "C" int pico_wad_gamemode(const char* name)
{
    return sniff_gamemode(name);
}

extern "C" int pico_wad_is_doom_family(const char* name)
{
    return sniff_is_doom_family(name) ? 1 : 0;
}

extern "C" const char* pico_wad_last(void)
{
    if (!g_last_loaded) {
        read_last_from_cfg();
        g_last_loaded = true;
    }
    return g_last;
}

namespace {

// Slot `line` (a full `key\t\tvalue\n` entry) into default.cfg under `key`.
void save_cfg_line(const char* cfg_key, const char* line)
{

    // Keep everything else the defaults file already carries; just slot this
    // key in (replace an existing line, append if absent). Reads whole-file
    // then rewrites -- the file is a handful of KB of text, fine for a one-
    // time boot op.
    std::vector<std::string> lines;
    if (FILE* f = fopen("default.cfg", "r")) {
        std::string cur;
        int ch;
        while ((ch = fgetc(f)) != EOF) {
            if (ch == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(static_cast<char>(ch));
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
        fclose(f);
    }

    std::string key(cfg_key);
    FILE* f = fopen("default.cfg", "w");
    if (!f)
        return; // engine's own M_SaveDefaults does the same silent return
    bool replaced = false;
    for (auto& l : lines) {
        if (!replaced && l.substr(0, key.size()) == key) {
            fputs(line, f);
            replaced = true;
        } else {
            fputs(l.c_str(), f);
            fputc('\n', f);
        }
    }
    if (!replaced)
        fputs(line, f);
    fclose(f);
}

} // namespace

extern "C" void pico_wad_save_last(const char* name)
{
    if (name == nullptr)
        return;
    char line[64];
    snprintf(line, sizeof(line), "%s\t\t\"%s\"\n", kCfgKey, name);
    save_cfg_line(kCfgKey, line);
}

// Same `key\t\t<int>` layout the engine's M_SaveDefaults() writes for ints.
extern "C" int pico_hires_setting(void)
{
    int val = kHiresDefault;
    if (FILE* f = fopen("default.cfg", "r")) {
        char key[80];
        char rest[100];
        while (fscanf(f, "%79s %[^\n]\n", key, rest) == 2) {
            if (strcmp(key, kHiresKey) == 0) {
                val = atoi(rest) != 0;
                break;
            }
        }
        fclose(f);
    }
    return val;
}

extern "C" void pico_hires_save(int hires)
{
    char line[48];
    snprintf(line, sizeof(line), "%s\t\t%d\n", kHiresKey, hires ? 1 : 0);
    save_cfg_line(kHiresKey, line);
}
