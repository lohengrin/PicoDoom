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
//
//    
//-----------------------------------------------------------------------------


#ifndef __D_EVENT__
#define __D_EVENT__


#include "doomtype.h"


//
// Event handling.
//

// Input event types.
typedef enum
{
    ev_keydown,
    ev_keyup,
    ev_mouse,
    ev_joystick
} evtype_t;

// Event structure. data1/2/3 pinned to an explicit width since this struct
// crosses from driver code (src/i_input_usbhid.cpp) into the engine's event
// queue (D_PostEvent/D_ProcessEvents, d_main.c) -- `type` stays the real
// evtype_t enum (not narrowed to a fixed-width type): GCC sizes an enum by
// its own declared values consistently on both this port's platforms (see
// docs/PLAN.md's type-size audit), so it isn't the same risk class as a
// plain `int`/`short`/`char` field would be.
typedef struct
{
    evtype_t	type;
    int32_t	data1;		// keys / mouse/joystick buttons
    int32_t	data2;		// mouse/joystick x move
    int32_t	data3;		// mouse/joystick y move
} event_t;

 
typedef enum
{
    ga_nothing,
    ga_loadlevel,
    ga_newgame,
    ga_loadgame,
    ga_savegame,
    ga_playdemo,
    ga_completed,
    ga_victory,
    ga_worlddone,
    ga_screenshot
} gameaction_t;



//
// Button/action code definitions.
//
typedef enum
{
    // Press "Fire".
    BT_ATTACK		= 1,
    // Use button, to open doors, activate switches.
    BT_USE		= 2,

    // Flag: game events, not really buttons.
    BT_SPECIAL		= 128,
    BT_SPECIALMASK	= 3,
    
    // Flag, weapon change pending.
    // If true, the next 3 bits hold weapon num.
    BT_CHANGE		= 4,
    // The 3bit weapon mask and shift, convenience.
    BT_WEAPONMASK	= (8+16+32),
    BT_WEAPONSHIFT	= 3,

    // Pause the game.
    BTS_PAUSE		= 1,
    // Save the game at each console.
    BTS_SAVEGAME	= 2,

    // Savegame slot numbers
    //  occupy the second byte of buttons.    
    BTS_SAVEMASK	= (4+8+16),
    BTS_SAVESHIFT 	= 2,
  
} buttoncode_t;




//
// GLOBAL VARIABLES
//
#define MAXEVENTS		64

extern  event_t		events[MAXEVENTS];
extern  int             eventhead;
extern	int		eventtail;

extern  gameaction_t    gameaction;


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
