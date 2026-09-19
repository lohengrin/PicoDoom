#include "i_frame_stats.hpp"

#include "pico/time.h"

namespace {
// Core0-only, single-writer-per-field (d_main.c/r_main.c write, one video
// backend's report_stats_if_due() reads+resets) -- plain accumulators, same
// convention as e.g. i_video_st7796.cpp's g_wait_us_in_window.
uint64_t g_tic_us = 0;
uint64_t g_draw2d_us = 0;
uint64_t g_bsp_walls_us = 0;
uint64_t g_planes_us = 0;
uint64_t g_sprites_us = 0;
uint32_t g_wad_cache_misses = 0;

float pct_of(uint64_t us, uint64_t elapsed_us) {
    return elapsed_us ? 100.0f * static_cast<float>(us) / static_cast<float>(elapsed_us) : 0.0f;
}
} // namespace

extern "C" {

uint64_t i_frame_stats_now_us(void) {
    return time_us_64();
}

void i_frame_stats_add_tic_us(uint64_t us) {
    g_tic_us += us;
}

void i_frame_stats_add_draw2d_us(uint64_t us) {
    g_draw2d_us += us;
}

void i_frame_stats_add_bsp_walls_us(uint64_t us) {
    g_bsp_walls_us += us;
}

void i_frame_stats_add_planes_us(uint64_t us) {
    g_planes_us += us;
}

void i_frame_stats_add_sprites_us(uint64_t us) {
    g_sprites_us += us;
}

void i_frame_stats_take(uint64_t elapsed_us, float* tic_pct, float* render3d_pct, float* draw2d_pct,
                         float* bsp_walls_pct, float* planes_pct, float* sprites_pct) {
    *tic_pct = pct_of(g_tic_us, elapsed_us);
    *draw2d_pct = pct_of(g_draw2d_us, elapsed_us);
    *bsp_walls_pct = pct_of(g_bsp_walls_us, elapsed_us);
    *planes_pct = pct_of(g_planes_us, elapsed_us);
    *sprites_pct = pct_of(g_sprites_us, elapsed_us);
    *render3d_pct = *bsp_walls_pct + *planes_pct + *sprites_pct;

    g_tic_us = 0;
    g_draw2d_us = 0;
    g_bsp_walls_us = 0;
    g_planes_us = 0;
    g_sprites_us = 0;
}

void i_frame_stats_inc_wad_cache_miss(void) {
    ++g_wad_cache_misses;
}

uint32_t i_frame_stats_take_wad_cache_misses(void) {
    uint32_t n = g_wad_cache_misses;
    g_wad_cache_misses = 0;
    return n;
}

} // extern "C"
