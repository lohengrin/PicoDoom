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
//	Status bar code.
//	Does the face/direction indicator animatin.
//	Does palette indicators as well (red pain/berserk, bright pickup)
//
//-----------------------------------------------------------------------------

#ifndef __STSTUFF_H__
#define __STSTUFF_H__

#include "doomtype.h"
#include "d_event.h"
#include "doomdef.h"	// SCREENWIDTH/HEIGHT, UI_SCALE, BASE_HEIGHT

// Size of statusbar.
// Now sensitive for scaling.
//
// ST_HEIGHT/ST_WIDTH/ST_Y are physical (post-UI_SCALE) pixel values: they
// address the real screens[] buffers directly (ST_WIDTH == SCREENWIDTH is
// also the buffer's own stride/allocation width, st_stuff.c's ST_refreshBackground
// V_CopyRect call and screens[4]'s Z_Malloc both need that). ST_Y_LOGICAL
// is the *un*-scaled 320x200-space counterpart, used only in st_lib.c to
// offset a widget's already-logical n->y before that offset itself gets
// UI_SCALE()'d -- n->y and ST_Y live in different spaces (physical vs
// logical) so they must never be subtracted from each other directly.
#define ST_HEIGHT	UI_SCALE(32*SCREEN_MUL)
#define ST_WIDTH	SCREENWIDTH
#define ST_Y		(SCREENHEIGHT - ST_HEIGHT)
#define ST_Y_LOGICAL	(BASE_HEIGHT - 32*SCREEN_MUL)


//
// STATUS BAR
//

// Called by main loop.
boolean ST_Responder (event_t* ev);

// Called by main loop.
void ST_Ticker (void);

// Called by main loop.
void ST_Drawer (boolean fullscreen, boolean refresh);

// Called when the console player is spawned on each level.
void ST_Start (void);

// Called by startup code.
void ST_Init (void);



// States for status bar code.
typedef enum
{
    AutomapState,
    FirstPersonState
    
} st_stateenum_t;


// States for the chat code.
typedef enum
{
    StartChatState,
    WaitDestState,
    GetChatState
    
} st_chatstateenum_t;


boolean ST_Responder(event_t* ev);



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
