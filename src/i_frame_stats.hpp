// Shared per-frame CPU-time breakdown, filled in from portable engine code
// (doom/d_main.c) and read back by whichever video backend is compiled in
// (i_video_ili9486.cpp / i_video_st7796.cpp / i_video_dvi.cpp) to extend its
// own 10s stats line. Exists because that line's existing core0-wait/
// core0-convert fields only cover I_FinishUpdate()'s own blit pipeline --
// they say nothing about the game tic / 3D render / 2D draw time that
// dominates each frame outside of it (see the 2026-09 FPS investigation:
// at native 480x300, ~70% of core0's per-frame time was unaccounted for by
// those two fields alone).
//
// Plain C interface (included from doom/d_main.c, portable C, as well as
// each C++ video backend) -- extern "C" linkage throughout, no classes.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Microsecond timestamp, same clock as each backend's own time_us_64()
// calls -- so a d_main.c span and a video-backend span are directly
// comparable/summable.
uint64_t i_frame_stats_now_us(void);

// Accumulate one measured span into its bucket. Called from doom/d_main.c
// around TryRunTics() (tic), and every other 2D drawing call in D_Display()
// -- ST_Drawer/AM_Drawer/HU_Drawer/M_Drawer/etc. (draw2d). Core0-only,
// single-threaded caller -- plain accumulation, no locking needed (same
// convention as the video backends' own stats counters).
void i_frame_stats_add_tic_us(uint64_t us);
void i_frame_stats_add_draw2d_us(uint64_t us);

// render3d's own three phases, from doom/r_main.c's R_RenderPlayerView():
// BSP traversal + wall/segment column drawing (R_RenderBSPNode -- classic
// linuxdoom draws walls interleaved with the BSP walk, not as a separate
// pass), floor/ceiling span drawing (R_DrawPlanes), and sprite + masked
// (two-sided) column drawing (R_DrawMasked). Their sum is what used to be
// reported as the single render3d bucket -- see i_frame_stats_take()'s
// render3d_pct, still reported as their total for continuity.
void i_frame_stats_add_bsp_walls_us(uint64_t us);
void i_frame_stats_add_planes_us(uint64_t us);
void i_frame_stats_add_sprites_us(uint64_t us);

// Called once per stats window by a video backend's own report_stats_if_due()
// -- converts each accumulator to a percentage of `elapsed_us` (that
// window's real duration) and resets all of them for the next window,
// mirroring how those backends already reset their own wait/convert
// accumulators. render3d_pct is the sum of bsp_walls/planes/sprites.
void i_frame_stats_take(uint64_t elapsed_us, float* tic_pct, float* render3d_pct, float* draw2d_pct,
                         float* bsp_walls_pct, float* planes_pct, float* sprites_pct);

// WAD lump cache miss counter (doom/w_wad.c's W_CacheLumpNum(): incremented
// whenever lumpcache[lump] was NULL, i.e. a purged/never-loaded lump had to
// be re-fetched via Z_Malloc+W_ReadLump -- on this port, an actual SD card
// read, not just a zone/PSRAM access. A raw per-window COUNT, not a
// percentage of frame time, since any nonzero rate here at all -- even a
// handful per second -- is the signal worth seeing (SD I/O latency is
// large enough that "how much of the frame" isn't the interesting
// question; "does this happen at all during normal play" is). 2026-09
// performance work: added to test the hypothesis that PU_CACHE eviction
// thrashing, not per-pixel/per-column CPU cost, explains why neither the
// R_DrawColumn merged-lookup-table change nor moving colormaps to SRAM
// measurably changed bsp+walls's cost.
void i_frame_stats_inc_wad_cache_miss(void);
// Returns the count accumulated since the last call and resets it, mirroring
// i_frame_stats_take()'s reset-on-read contract.
uint32_t i_frame_stats_take_wad_cache_misses(void);

#ifdef __cplusplus
}
#endif
