#pragma once

// Phase 4.5 (performance): the LCD blit (palette->RGB565 + DMA-fed SPI
// push, see i_video_ili9486.cpp) now runs on core1 instead of core0, so
// core0 is free to move straight on to the next tic's game logic/render
// instead of blocking on the ~41ms SPI transfer. core1 already runs
// PicoUsbKeyboard's tuh_task() loop (see PicoUsbKeyboard.cpp) -- that loop
// calls this once per iteration, interleaved with tuh_task(), so a pending
// blit lands as many small (one-row) chunks rather than one long blocking
// call that would starve Pico-PIO-USB's software-timed bus servicing for
// the whole transfer.
//
// No-op (returns immediately) when no frame is pending or mid-row-nothing
// to do -- safe to call unconditionally every loop iteration.
void i_video_core1_step();
