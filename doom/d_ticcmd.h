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
// DESCRIPTION:
//	System specific interface stuff.
//
//-----------------------------------------------------------------------------


#ifndef __D_TICCMD__
#define __D_TICCMD__

#include "doomtype.h"

#ifdef __GNUG__
#pragma interface
#endif

// The data sampled per tick (single player)
// and transmitted to other peers (multiplayer).
// Mainly movements/button commands per game tick,
// plus a checksum for internal state consistency.
typedef struct
{
    // int8_t, not plain char: G_BuildTiccmd (g_game.c) writes negative
    // values here directly (cmd->forwardmove += forward, forward can be
    // negative -- backward/left movement), and G_ReadDemoTiccmd already
    // explicitly casts to (signed char) when reading these same fields back
    // from a demo buffer, confirming the original intent. Plain `char` is
    // signed by default on x86 Linux GCC but UNSIGNED by default on this
    // ARM EABI toolchain (confirmed via a CHAR_MIN<0 compile-time test,
    // during Phase 2's type-size audit) -- under that default here, a small
    // negative move wrapped to a large unsigned value near 256, misread
    // downstream as a large *positive* (forward) move: backward became
    // impossible, replaced by an always-near-max-speed forward, regardless
    // of how small the original negative delta was (matches the reported
    // "insensitive to sensitivity" symptom exactly, since -1 and -5 both
    // wrap to values near 256 either way).
    int8_t	forwardmove;	// *2048 for move
    int8_t	sidemove;	// *2048 for move
    int16_t	angleturn;	// <<16 for angle delta
    int16_t	consistancy;	// checks for net game
    uint8_t	chatchar;
    uint8_t	buttons;
} ticcmd_t;



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
