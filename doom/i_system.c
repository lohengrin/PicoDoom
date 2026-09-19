// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"




int	mb_used = 6;

#ifdef PICO
// Zone heap lives in PSRAM (8 MB on the RP2350-PiZero): an RP2350's ~500 KB
// SRAM cannot back DOOM's 6 MB default zone. psram_malloc is provided by
// Pico-Toolset's pico_toolset_psram component (third_party/pico-toolset);
// pico_toolset::psram_init() runs in main() (src/PicoDoom.cpp) before
// D_DoomMain().
extern void *psram_malloc (size_t);

#include <malloc.h>
extern char __StackLimit;
extern char __bss_end__;

// Small, always-fully-resident, extremely hot pieces of otherwise-PSRAM-
// backed zone data (see I_ZoneBase() below) can be worth pinning in SRAM
// instead -- see doom/r_data.c's R_InitColormaps() for the motivating case
// (2026-09 performance work). A plain malloc() is the wrong way to attempt
// this: this SDK's pico_malloc wrapper has PICO_MALLOC_PANIC=1 by default,
// so a malloc() call that's going to fail panics immediately instead of
// returning NULL -- same reasoning, and same mallinfo()-based headroom
// check (uordblks vs the heap arena size, NOT fordblks -- see
// src/i_video_st7796.cpp's identically-shaped comment for why fordblks is
// wrong here), as the video backends already use for their blit buffers.
// Returns NULL (never panics) if there isn't enough estimated headroom for
// `size`; a real SRAM allocation otherwise.
void *I_TrySramMalloc (size_t size)
{
    struct mallinfo mi = mallinfo();
    size_t sram_total = (size_t)(&__StackLimit - &__bss_end__);
    size_t sram_used = (size_t)mi.uordblks;
    size_t sram_free_estimate = sram_total > sram_used ? sram_total - sram_used : 0;
    if (sram_free_estimate < size)
	return NULL;
    return malloc (size);
}
#endif


void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    *size = mb_used*1024*1024;
#ifdef PICO
    return (byte *) psram_malloc (*size);
#else
    return (byte *) malloc (*size);
#endif
}



//
// I_GetTime
// returns time in 1/70th second tics
//
int  I_GetTime (void)
{
    struct timeval	tp;
    struct timezone	tzp;
    int			newtics;
    static int		basetime=0;
  
    gettimeofday(&tp, &tzp);
    if (!basetime)
	basetime = tp.tv_sec;
    newtics = (tp.tv_sec-basetime)*TICRATE + tp.tv_usec*TICRATE/1000000;
    return newtics;
}



//
// I_Init
//
void I_Init (void)
{
    I_InitSound();
    //  I_InitGraphics();
}

#ifdef PICO
// Forward-declared rather than #include "hardware/watchdog.h" -- this file
// compiles as gnu90 (see CMakeLists.txt), and Pico SDK headers generally
// need C11 (same reason d_main.c gets its own carve-out there). pc=0/sp=0
// means "standard boot" (jump to the normal reset vector, same as a power
// cycle) rather than resuming at a specific address.
extern void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms);
#endif

//
// I_Quit
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();
#ifdef PICO
    // No OS to return to -- exit()/_exit() has nothing meaningful to do on
    // bare metal. Full chip reboot instead: the watchdog fires after
    // delay_ms and the board comes back up through the normal boot sequence,
    // same as a power cycle.
    printf("PicoDoom: quitting -- rebooting...\n");
    watchdog_reboot(0, 0, 100);
    for (;;)
	; // watchdog_reboot() only arms the reset; wait for it to fire.
#else
    exit(0);
#endif
}

void I_WaitVBL(int count)
{
#ifdef SGI
    sginap(1);                                           
#else
#ifdef SUN
    sleep(0);
#else
    usleep (count * (1000000/70) );                                
#endif
#endif
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;

#ifdef PICO
    // screens[] (SCREENWIDTH*SCREENHEIGHT*4 = 250 KB) does not fit in the
    // ~170 KB left of the RP2350's 512 KB SRAM after DOOM's own static
    // tables and the SDK/USB/FatFs globals -- same reasoning as I_ZoneBase's
    // PSRAM-backed zone heap above.
    mem = (byte *)psram_malloc ((size_t)length);
#else
    mem = (byte *)malloc (length);
#endif
    if (!mem)
	I_Error ("I_AllocLow: alloc(%d) failed", length);
    memset (mem,0,length);
    return mem;
}


//
// I_Error
//
extern boolean demorecording;

void I_Error (char *error, ...)
{
    va_list	argptr;

    // Message first.
    va_start (argptr,error);
    fprintf (stderr, "Error: ");
    vfprintf (stderr,error,argptr);
    fprintf (stderr, "\n");
    va_end (argptr);

    fflush( stderr );

    // Shutdown. Here might be other errors.
    if (demorecording)
	G_CheckDemoStatus();

    D_QuitNetGame ();
    I_ShutdownGraphics();

#ifdef PICO
    /* No OS and no safe bare-metal exit under PICO: newlib's exit() ->
     * _exit() is the crt0 fallback (a `bkpt`), which with no debugger
     * attached escalates to HardFault -> pico_toolset's fault handler ->
     * watchdog reboot -- silently erasing the very "Error:" line that
     * identifies this bug. Hang right here instead so the message stays on
     * the USB-CDC console for diagnosis (same reasoning as PLAN.md's I_Quit
     * note). */
    printf ("\nPicoDoom: engine error -- device halted\n");
    fflush (stdout);
    for (;;)
	;
#else
    exit(-1);
#endif
}
