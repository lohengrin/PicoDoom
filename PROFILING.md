# Profiling PicoDoom

Two complementary tools exist for finding where CPU time actually goes,
built during the 2026-09 FPS investigation on the `lcd-st7796` build (native
480x300). Use the coarse one first -- it's always available, needs no extra
hardware, and tells you *which phase* of a frame is expensive. Use the real
profiler when you need to know *which function inside that phase*, or when
the coarse numbers don't explain something (see "When the coarse stats
aren't enough" below -- this happened twice in the original investigation).

## 1. Coarse per-frame phase stats (always available)

Every video backend (`src/i_video_ili9486.cpp`, `src/i_video_st7796.cpp`,
`src/i_video_dvi.cpp`) prints a line to the serial console every 10 seconds:

```
PicoDoom: SRAM 198/295KB  PSRAM 7046/8192KB  FPS 22.1  core0-wait 9%  core0-convert 22%  core1-dma 48%  tic 3%  render3d 60% (bsp+walls 44%  planes 14%  sprites 2%)  draw2d 3%  wad-cache-miss 0/10s
```

No setup needed -- just connect over USB serial (the same CDC port used for
the `pclk`/`gamma`/`mouse`/`sens` console commands, see `src/i_serial_console.cpp`)
and watch the output while playing.

Field meanings:

| Field | Meaning |
|---|---|
| `SRAM used/total` | newlib heap usage (mallinfo `uordblks`) vs. the arena between `__bss_end__` and `__StackLimit` |
| `PSRAM used/total` | psram_malloc pool usage |
| `FPS` | frames rendered in the last 10s window / window duration |
| `core0-wait` | % of that window core0 spent blocked waiting for a free ping-pong blit buffer slot (LCD builds only) |
| `core0-convert` | % of that window core0 spent doing the palette->RGB565 conversion in `I_FinishUpdate()` (LCD builds only) |
| `core1-dma` | % of that window core1 spent actually pushing bytes over SPI (LCD builds only) |
| `tic` | % spent in game-logic tic processing (`TryRunTics()`/`singletics` block, `doom/d_main.c`'s `D_DoomLoop()`) |
| `render3d` | % spent in `R_RenderPlayerView()` overall -- the sum of the three sub-phases in parentheses |
| `bsp+walls` | % spent in `R_RenderBSPNode()` -- BSP traversal AND wall/column drawing interleaved (classic linuxdoom draws walls during the BSP walk, not as a separate pass) |
| `planes` | % spent in `R_DrawPlanes()` -- floor/ceiling span drawing |
| `sprites` | % spent in `R_DrawMasked()` -- sprites + masked (two-sided) column drawing |
| `draw2d` | % spent in every other 2D drawing call in `D_Display()` (status bar, automap, HUD, menus, intermission, wipes) |
| `wad-cache-miss` | raw COUNT (not %) of `W_CacheLumpNum()` calls in the last 10s that found `lumpcache[lump]` empty and had to reload the lump (an actual SD card read on this port) -- any nonzero rate during steady gameplay is the signal worth noticing; a burst at level-load time is normal |

Implementation lives in `src/i_frame_stats.hpp`/`.cpp` (a small, portable-C-callable
module: `doom/d_main.c` and `doom/r_main.c` call its `i_frame_stats_add_*_us()`
functions around the phases they own; `doom/w_wad.c` calls
`i_frame_stats_inc_wad_cache_miss()`; each video backend calls
`i_frame_stats_take(...)` once per stats window to read-and-reset all of them
alongside its own wait/convert/dma counters).

### Adding a new phase bucket

1. Add an accumulator + `i_frame_stats_add_<name>_us()` / take-and-reset
   plumbing in `src/i_frame_stats.hpp` and `.cpp`, following the existing
   fields as a template.
2. Wrap the code span you care about with
   `uint64_t start = i_frame_stats_now_us(); ...; i_frame_stats_add_<name>_us(i_frame_stats_now_us() - start);`
   at the call site (portable C files like `doom/d_main.c`/`r_main.c` can
   call these directly -- the header uses plain `extern "C"` declarations).
3. Add the new field to the `printf()` in all three `i_video_*.cpp` backends
   (or just the one(s) you're testing on, but keep them in sync).

## 2. Real sampling profiler over a debug probe (`tools/profile_sample.py`)

### When the coarse stats aren't enough

The phase buckets above tell you *which phase* is expensive, but not *which
function inside it*, and they can't catch anything the phase boundaries
don't already anticipate. Twice during the original investigation the coarse
numbers were not enough to make progress:

- Two independent, plausible-looking optimizations (a merged texture/colormap
  lookup table in `R_DrawColumn`, and moving the colormap table to SRAM)
  each showed **zero measurable change** in the `bsp+walls` percentage. That
  result alone doesn't tell you *why* -- it could mean the hypothesis was
  wrong, or that the fix's effect was too small to see against noise, or
  that it wasn't exercised in the tested scene. Real sampling settled it:
  `R_DrawColumn` really is ~29% of every frame regardless, and the
  optimization's own threshold (only worth it for columns taller than a
  break-even point) was hiding its effect in scenes with mostly short
  columns.
- The coarse stats showed a `wad-cache-miss` spike that looked alarming
  until real profiling (and a second, controlled sampling run) made clear
  it was a one-time level-load burst, not steady-state thrashing --
  something a raw count-per-window can't distinguish from a real problem.

If a change to a hot function shows no effect in the coarse stats, or a
percentage doesn't make sense, reach for this tool before concluding
anything.

### Prerequisites

- A debug probe wired to the target's SWD pins (SWDIO/SWCLK/GND) in addition
  to its normal USB power/serial connection -- the official Raspberry Pi
  Debug Probe (CMSIS-DAP) is what this was built and validated against.
- `openocd` and `arm-none-eabi-addr2line` on `PATH` (both already required
  to build/flash this project with the standard toolchain).
- The `.elf` for whichever build variant is actually running on the board
  right now (e.g. `build-st7796/PicoDoom.elf`) -- symbol resolution needs to
  match what's flashed, or you'll get garbage or `??` results.

### How it works

The technique is the classic "poor man's profiler": repeatedly halt the
CPU, read the program counter, resume, and build a histogram of where it
stopped. Over enough samples (thousands, during real gameplay) the
distribution converges on where time is actually spent, function by
function -- no OS, no `perf`/`ptrace`, no instrumentation of the target
binary required. Each halt/resume pauses execution for a few milliseconds
(visible as brief stutter/audio glitches), which is why you profile *while
actually playing* the scenario you care about, for long enough that the
brief pauses average out statistically.

### Step by step

1. Start OpenOCD (leave it running in its own terminal):
   ```bash
   openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg
   ```
   (swap `cmsis-dap.cfg` for your probe's own interface config if it's not
   the official Raspberry Pi Debug Probe). This opens a GDB server on
   `:3333` and the Tcl RPC port this script actually talks to on `:6666`.

2. Flash the build you want profiled the normal way first (this script only
   samples an already-running target, it never flashes anything).

3. Find the OpenOCD target name for **core0** -- RP2350 exposes both Arm
   cores as separate targets, and game logic/rendering all runs on core0
   (core1 only pumps DMA + services USB-PIO on LCD builds). With OpenOCD
   still running, in another terminal:
   ```bash
   telnet localhost 4444
   ```
   then at the prompt, type `targets`. Expect something like:
   ```
        TargetName         Type       Endian TapName            State
   --  ------------------ ---------- ------ ------------------ ------------
    0* rp2350.cm0         cortex_m   little rp2350.cpu         running
    1  rp2350.cm1         cortex_m   little rp2350.cpu         running
   ```
   `rp2350.cm0` is core0 -- that's the name to pass to `--target` below.
   (This is board/OpenOCD-version-dependent; always check with `targets`
   rather than assuming the name.)

4. While actually playing/running the scenario you want profiled, in a
   third terminal:
   ```bash
   python3 tools/profile_sample.py \
       --elf build-st7796/PicoDoom.elf \
       --target rp2350.cm0 \
       --samples 3000 --interval 0.003
   ```
   Keep doing the thing you want profiled (stand in one spot for a "pure
   rendering, no game logic" read, or play normally for a representative
   mix) until it finishes -- it prints progress every 200 samples and can
   take noticeably longer than `samples * interval` suggests, since each
   halt/resume round-trip has real SWD latency on top of the requested
   interval. It's normal for 3000 samples at a nominal 3ms apart to take
   well over a minute of wall-clock time.

5. Read the histogram: percentage and raw sample count per function, sorted
   descending. A function with N% of samples is a good estimate of that
   function spending N% of total core0 time, *including* time spent in
   anything it inlines.

### Gotcha: per-target command syntax

OpenOCD's Tcl RPC does **not** accept `<target-name> <command>` as a
per-command prefix for `halt`/`reg`/`resume` -- that silently returns an
empty response (this cost real debugging time to find). The right pattern
is to *switch* the current target once with `targets <name>`, then issue
plain `halt`/`reg pc`/`resume` against whatever's now current. The script
already does this correctly if you pass `--target`; this note is here in
case you're scripting something new against the same RPC interface.

### Drilling into a surprising result

If a function shows up with a suspicious/surprising percentage (e.g. one
that should only run at load time, not every frame), get the full inline
chain for its samples instead of just the top-level name:

```bash
python3 tools/profile_sample.py --elf build-st7796/PicoDoom.elf \
    --target rp2350.cm0 --samples 1500 \
    --drill-down "SomeSuspiciousFunction"
```

This re-resolves every matching sample's PC with `addr2line -i` (showing
the full inline chain, not just the outermost frame) and groups identical
chains together with an example address, so you can tell a genuine
standalone hot function apart from one that's actually inlined into
something else and only *looks* like it's the direct cost.

Add `--drill-down-lr` to also capture and resolve `$lr` at each matching
sample, in an attempt to identify the *caller*. The script masks bit 0 off
both `pc` and `lr` before resolving (M-profile ARM stores a return address
*as data* with bit 0 set, to indicate Thumb state on the eventual `bx`;
feeding that odd value straight to `addr2line`/`objdump` looks up the
wrong, misaligned address and silently fails -- found the hard way).

**Caveat, found the hard way (and confirmed, not just suspected)**: this
only works if the matched function is a real leaf (calls nothing else
after the point you're sampling) that hasn't repurposed `lr` as an
ordinary scratch register for its own use -- which a leaf function is
entirely free to do, since it never needs `lr` to hold a valid return
address until its own final `bx lr`. Confirmed exactly this happening with
`W_CheckNumForName()` (see "Known open item" below): `--drill-down-lr`
resolved every sample to the same fixed address, which turned out to be
newlib's `_ctype_` table -- a **data** symbol, not a return address at
all -- because `strupr()` (called once, early in the function, using
`toupper()`/`_ctype_` internally) left that table's address cached in
`lr` for the rest of the function's lifetime, since nothing after it needs
`lr` for anything else. If `--drill-down-lr` resolves to `??`, or to an
address that turns out to be a data symbol via
`arm-none-eabi-nm -n <elf> | grep <addr>` (always check before trusting
it), that's what's happening, and finding the real caller needs genuine
stack-memory unwinding instead (not currently implemented in this script).

### Known open item

`W_CheckNumForName()` (an O(numlumps) linear WAD-directory scan,
`doom/w_wad.c`) shows up at a real, reproducible ~4% of samples during
confirmed steady, no-level-transition gameplay -- genuinely executing
inside its own loop (confirmed via `--drill-down`), not a misattribution
artifact. No caller in the whole codebase calls it (directly or via
`R_FlatNumForName`/`R_TextureNumForName`) anywhere outside one-time
level-load/init code, as far as a full-codebase text search found, and
`--drill-down-lr` resolved to newlib's `_ctype_` table rather than a real
caller -- see the caveat above for why. Left unresolved as a low-priority
item (dwarfed by `R_DrawColumn` and `I_FinishUpdate` at ~29%/~25% each) --
picking it back up would mean adding real stack unwinding to this script.
