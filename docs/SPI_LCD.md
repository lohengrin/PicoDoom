# SPI LCD builds (`lcd` ILI9486, `lcd-st7796` ST7796U)

(Technical reference for the SPI LCD video path; the project map is in [AGENTS.md](../AGENTS.md).)

- DOOM renderer writes `screens[0]`: 320×200, palette-indexed (indices 0–255).
  **`screens[0]`'s address is never touched by this file** — Phase 4.6 tried
  ping-ponging it directly between two buffers and reverted (Phase 4.6.2,
  see docs/PLAN.md): several engine subsystems assume `screens[0]`'s
  *content*, not just its address, persists across many tics — not just a
  pointer-cache problem like `R_InitBuffer()` (below), which *can* be
  refreshed. `doom/st_stuff.c`'s status bar does incremental "diff" redraws
  that skip repainting unchanged widgets, trusting last tic's pixels are
  still there; `doom/f_wipe.c`'s level-transition melt effect caches
  `screens[0]`'s pointer *once* at the start of a ~30-tic animation and
  keeps writing into that same captured address the whole time. Ping-ponging
  `screens[0]` broke both (confirmed on hardware: corrupted/blinking status
  bar and transition screens) — there's no per-call fix for this the way
  `R_InitBuffer()` has one; a ping-ponged buffer just can't provide
  frame-to-frame content continuity.
- `I_SetPalette()` receives the PLAYPAL lump (256×3 RGB, full 8-bit values) each time
  the palette changes; rebuilds a 256-entry RGB565 (wire/big-endian) LUT via
  `gammatable[usegamma]`, matching the reference X11 driver's `UploadNewPalette`.
- **`screens[0]` lives in SRAM on LCD builds** (`doom/v_video.c` `V_Init`, via
  `I_TrySramMalloc`, PSRAM fallback with a boot-log message; `screens[1..3]` stay in
  PSRAM). Measured: moving it from PSRAM took 25.3 -> 31.0 FPS (column writes with a
  480-byte stride and the convert read were PSRAM-latency-bound). HDMI is unchanged.
- **Blit pipeline** (`I_FinishUpdate`, core0 -> core1): per 25-row band, hash the raw
  `screens[0]` indices; if equal to the last band actually sent, skip it entirely (no
  convert, no DMA -- the panel keeps its GRAM). Otherwise convert to the wire-order
  RGB565 LUT (word-wide: 4 source px per load, 2 dest px per store) into one of
  `kNumSlots`=5 SRAM slots (24 KB each) and push `(band<<3)|slot` over the SDK FIFO;
  core1's `i_video_core1_step()` `set_window` + DMAs it. Core0 blocks only when all slots
  are in flight. **Hashes must be invalidated** (`invalidate_band_hashes()`) by anything
  that changes index->color mapping or repaints the panel behind the game's back
  (palette/gamma rebuild, `i_video_lcd_set_hires()` fill). Standing still: ~88% of
  bands skipped, `core1-dma` 71% -> 9%. Slot count is bounded by SRAM left after
  `screens[0]` (~269/289 KB heap used).
- **Do NOT write the converted frame to PSRAM** (tried 2026-10, reverted): decoupling
  core0 from the SPI drain with two 288 KB full-frame PSRAM slots made `core0-convert`
  jump 3 -> 24 ms (PSRAM writes ~12 MB/s, one QSPI transaction per store) and FPS drop
  31 -> 19. PSRAM reads are fine; PSRAM *writes* are the slow path. Same lesson applies
  to any per-frame write-heavy buffer.
- Other perf-pass results (all hardware-verified): `ylookup`/`columnofs` are static
  SRAM arrays (~3 KB; were PSRAM pools, ~1000+ random reads/frame); hot renderer
  code runs from RAM via `PICO_RAMFUNC()` (`__not_in_flash_func` wrapper, defined at the
  top of each file): `R_RenderSegLoop`, `R_MapPlane`, `R_MakeSpans`, `R_DrawPlanes`,
  `R_GetColumn`, `Z_ChangeTag2`, `W_CacheLumpNum` (plus the five `r_draw.c` loops from
  Phase 4.7) -- the 16 KB XIP cache is shared with PSRAM data traffic, so flash-resident
  hot code gets evicted constantly. SPI clock is already at its hardware ceiling
  (clk_peri/2 = 132 MHz); raising it needs a higher clk_sys.
- Ideas audited but not done: `FixedDiv2` uses soft-`double` (M33 FPU is single-precision;
  few calls/frame, low value); `W_CheckNumForName` O(n) scan shows ~4% in profiles (hash
  table would fix it regardless of caller, see PROFILING.md); `screens[0]` at 144 KB leaves
  no room for more blit slots.
- `r_draw.c`'s `R_InitBuffer()` caches per-row pointers *into* `screens[0]`
  (`ylookup[i] = screens[0] + ...`), refreshed only on view-size changes (the
  menu's screen-size +/- keys) — moot now that `screens[0]` never moves, but
  the reason this file must never start reassigning `screens[0]` again without
  re-reading the Phase 4.6.2 history first.
- `pico_toolset::Ili9486` (Pico-Toolset submodule) — low-level SPI1 driver, originally
  ported verbatim from the validated TOM6809 project (shift-register wire protocol,
  panel init sequence) and since moved to Pico-Toolset (Phase 2); needs C++20
  (`std::span`), hence `CMAKE_CXX_STANDARD 20`. Only ever touched from core0 during the
  one-time `I_InitGraphics()`/`fill_solid()` bring-up call and from core1 thereafter
  (`i_video_core1_step()`) — never concurrently.
- Stats line (10s interval, see PROFILING.md for every field): `core0-wait%`
  (backpressure -- core0 blocked on a free slot), `core0-convert%` (palette->RGB565
  convert), `core1-dma%` (SPI feed), per-phase `tic/render3d/draw2d`, and
  `bands-skipped%`. See
  docs/PLAN.md's Phase 4.5/4.6 for the full history: a row-batching attempt
  (`kRowsPerChunk=8`, since reverted to 1) made things both slower *and* caused a
  hang after ~20s — plausibly Pico-PIO-USB's software-timed bus servicing needing
  `tuh_task()` called more often than one chunk's worth of convert+DMA time allowed,
  though unconfirmed.
- `doom/tables.c`'s `finesine`/`finetangent`/`tantoangle` (~64KB) are `const` under
  `#ifdef PICO` (`.rodata`/flash instead of `.data`/SRAM) — confirmed safe by finding
  that `R_InitPointToAngle()`/`R_InitTables()`, the only code that ever assigns to
  them, are both `#if 0`'d out by id Software themselves. This is what funded
  `screens[0]`'s move to SRAM above; see
  docs/PLAN.md's Phase 4.6 memory audit for what was and wasn't safe to relocate
  (most of DOOM's big SRAM users are hot renderer state — `visplanes`, `openings`,
  etc. — genuinely not movable without slowing the renderer).
