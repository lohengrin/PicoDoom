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
//  Internally used data structures for virtually everything,
//   key definitions, lots of other stuff.
//
//-----------------------------------------------------------------------------

#ifndef __DOOMDEF__
#define __DOOMDEF__

#include <stdio.h>
#include <string.h>

//
// Global parameters/defines.
//
// DOOM version
// Demo/savegame compatibility byte -- must match the version byte v1.9
// IWADs actually embed in their demo lumps (verified against wad/DOOM.WAD's
// DEMO1/2/3, all 109), not the "linuxdoom-1.10" source release name.
enum { VERSION =  109 };


// Game mode handling - identify IWAD version
//  to handle IWAD dependend animations etc.
typedef enum
{
  shareware,	// DOOM 1 shareware, E1, M9
  registered,	// DOOM 1 registered, E3, M27
  commercial,	// DOOM 2 retail, E1 M34
  // DOOM 2 german edition not handled
  retail,	// DOOM 1 retail, E4, M36
  indetermined	// Well, no IWAD found.
  
} GameMode_t;


// Mission packs - might be useful for TC stuff?
typedef enum
{
  doom,		// DOOM 1
  doom2,	// DOOM 2
  pack_tnt,	// TNT mission pack
  pack_plut,	// Plutonia pack
  none

} GameMission_t;


// Identify language to use, software localization.
typedef enum
{
  english,
  french,
  german,
  unknown

} Language_t;


// If rangecheck is undefined,
// most parameter validation debugging code will not be compiled
#define RANGECHECK

// Do or do not use external soundserver.
// The sndserver binary to be run separately
//  has been introduced by Dave Taylor.
// The integrated sound support is experimental,
//  and unfinished. Default is synchronous.
// Experimental asynchronous timer based is
//  handled by SNDINTR. 
#define SNDSERV  1
//#define SNDINTR  1


// This one switches between MIT SHM (no proper mouse)
// and XFree86 DGA (mickey sampling). The original
// linuxdoom used SHM, which is default.
//#define X11_DGA		1


//
// For resize of screen, at start of game.
// It will not work dynamically, see visplanes.
//
#define	BASE_WIDTH		320
#define	BASE_HEIGHT		200

// Uniform scale factor from the classic 320x200 "logical" UI coordinate
// space (patch positions, status bar/HUD/menu layout constants -- all
// authored assuming 320x200) to the build's actual physical SCREENWIDTH/
// SCREENHEIGHT. Identity at 320x200; exact x3/2 at 480x300 (an exact uniform
// x3/2 scale of 320x200, see SCREENWIDTH/SCREENHEIGHT below).
// UI_SCALE(v) converts one logical-space coordinate or length to its
// physical equivalent; used by V_DrawPatch/V_DrawPatchFlipped
// (doom/v_video.c) and by the handful of call sites elsewhere that need to
// convert an already-logical value before feeding it to a physical-space
// primitive (V_CopyRect, view-border sizing in doom/r_main.c/doom/r_draw.c).
//
// hdmi: compile-time constants exactly as before. lcd: derived from the
// runtime SCREENWIDTH (see below) -- the division is by the constant
// BASE_WIDTH, which GCC turns into a multiply-by-reciprocal, not a real
// divide, so this costs one extra multiply per call over the old constant
// x3/2. Width alone is enough (not height): both 320x200 and 480x300 are
// uniform scales of the 320x200 base, so the two axes always agree.
#ifdef PICODOOM_HDMI
#define UI_SCALE_NUM 1
#define UI_SCALE_DEN 1
#define UI_SCALE(v) (((v)*UI_SCALE_NUM)/UI_SCALE_DEN)
#else
#define UI_SCALE(v) (((v)*SCREENWIDTH)/BASE_WIDTH)
#endif

// It is educational but futile to change this
//  scaling e.g. to 2. Drawing of status bar,
//  menues etc. is tied to the scale implied
//  by the graphics.
#define	SCREEN_MUL		1
#define	INV_ASPECT_RATIO	0.625 // 0.75, ideally

// Defines suck. C sucks.
// C++ might sucks for OOP, but it sure is a better C.
// So there.
//
// The hdmi build stays at vanilla 320x200 (its DVI canvas/TMDS encoding is
// hardcoded around that, see src/i_video_dvi.cpp) -- only the lcd builds can
// go native. 480x300 is an *exact* uniform x3/2 scale of 320x200 (480/320=1.5,
// 300/200=1.5) that reduces to the same 8:5 aspect ratio as INV_ASPECT_RATIO
// below, so no view-geometry math changes are needed, and 2D UI patches
// (status bar/HUD/menus) can use one clean x3/2 scale factor consistently
// with the 3D view. The LCD panels are physically 480x320; the leftover
// 20 rows are a small top/bottom letterbox in the LCD driver's blit step.
//
// lcd builds choose between 320x200 and 480x300 at BOOT (the "High res"
// checkbox in the WAD-selection menu, src/wad_menu.cpp), so on those builds
// SCREENWIDTH/SCREENHEIGHT are runtime globals -- set once by
// i_video_lcd_set_hires() (src/i_video_*.cpp) before D_DoomMain() runs and
// never changed afterwards -- and every fixed-size array/pool that used to be
// sized by them is sized by MAX_SCREENWIDTH/MAX_SCREENHEIGHT instead (the
// largest mode; the smaller mode simply leaves the tail of each array
// unused). Anything that needs a compile-time constant (array bounds,
// static initializers, #if) must use MAX_SCREEN* or compute at runtime.
//
// One thing to keep in mind when writing a hot loop against these: SCREENWIDTH
// is now a memory load, and GCC must assume a byte store through `dest` may
// alias it (this code builds -fno-strict-aliasing), so it re-loads it every
// iteration unless you copy it into a local first -- see R_DrawColumn.
// (hdmi's constant is still folded away as before, so the copy is free there.)
#ifdef PICODOOM_HDMI
#define MAX_SCREENWIDTH  320
#define MAX_SCREENHEIGHT 200
#define SCREENWIDTH  320
//SCREEN_MUL*BASE_WIDTH //320
#define SCREENHEIGHT 200
//(int)(SCREEN_MUL*BASE_WIDTH*INV_ASPECT_RATIO) //200
#else
#define MAX_SCREENWIDTH  480
#define MAX_SCREENHEIGHT 300
extern int g_screenwidth;
extern int g_screenheight;
#define SCREENWIDTH  g_screenwidth
#define SCREENHEIGHT g_screenheight
#endif




// The maximum number of players, multiplayer/networking.
#define MAXPLAYERS		4

// State updates, number of tics / second.
#define TICRATE		35

// The current state of the game: whether we are
// playing, gazing at the intermission screen,
// the game final animation, or a demo. 
typedef enum
{
    GS_LEVEL,
    GS_INTERMISSION,
    GS_FINALE,
    GS_DEMOSCREEN
} gamestate_t;

//
// Difficulty/skill settings/filters.
//

// Skill flags.
#define	MTF_EASY		1
#define	MTF_NORMAL		2
#define	MTF_HARD		4

// Deaf monsters/do not react to sound.
#define	MTF_AMBUSH		8

typedef enum
{
    sk_baby,
    sk_easy,
    sk_medium,
    sk_hard,
    sk_nightmare
} skill_t;




//
// Key cards.
//
typedef enum
{
    it_bluecard,
    it_yellowcard,
    it_redcard,
    it_blueskull,
    it_yellowskull,
    it_redskull,
    
    NUMCARDS
    
} card_t;



// The defined weapons,
//  including a marker indicating
//  user has not changed weapon.
typedef enum
{
    wp_fist,
    wp_pistol,
    wp_shotgun,
    wp_chaingun,
    wp_missile,
    wp_plasma,
    wp_bfg,
    wp_chainsaw,
    wp_supershotgun,

    NUMWEAPONS,
    
    // No pending weapon change.
    wp_nochange

} weapontype_t;


// Ammunition types defined.
typedef enum
{
    am_clip,	// Pistol / chaingun ammo.
    am_shell,	// Shotgun / double barreled shotgun.
    am_cell,	// Plasma rifle, BFG.
    am_misl,	// Missile launcher.
    NUMAMMO,
    am_noammo	// Unlimited for chainsaw / fist.	

} ammotype_t;


// Power up artifacts.
typedef enum
{
    pw_invulnerability,
    pw_strength,
    pw_invisibility,
    pw_ironfeet,
    pw_allmap,
    pw_infrared,
    NUMPOWERS
    
} powertype_t;



//
// Power up durations,
//  how many seconds till expiration,
//  assuming TICRATE is 35 ticks/second.
//
typedef enum
{
    INVULNTICS	= (30*TICRATE),
    INVISTICS	= (60*TICRATE),
    INFRATICS	= (120*TICRATE),
    IRONTICS	= (60*TICRATE)
    
} powerduration_t;




//
// DOOM keyboard definition.
// This is the stuff configured by Setup.Exe.
// Most key data are simple ascii (uppercased).
//
#define KEY_RIGHTARROW	0xae
#define KEY_LEFTARROW	0xac
#define KEY_UPARROW	0xad
#define KEY_DOWNARROW	0xaf
#define KEY_ESCAPE	27
#define KEY_ENTER	13
#define KEY_TAB		9
#define KEY_F1		(0x80+0x3b)
#define KEY_F2		(0x80+0x3c)
#define KEY_F3		(0x80+0x3d)
#define KEY_F4		(0x80+0x3e)
#define KEY_F5		(0x80+0x3f)
#define KEY_F6		(0x80+0x40)
#define KEY_F7		(0x80+0x41)
#define KEY_F8		(0x80+0x42)
#define KEY_F9		(0x80+0x43)
#define KEY_F10		(0x80+0x44)
#define KEY_F11		(0x80+0x57)
#define KEY_F12		(0x80+0x58)

#define KEY_BACKSPACE	127
#define KEY_PAUSE	0xff

#define KEY_EQUALS	0x3d
#define KEY_MINUS	0x2d

#define KEY_RSHIFT	(0x80+0x36)
#define KEY_RCTRL	(0x80+0x1d)
#define KEY_RALT	(0x80+0x38)

#define KEY_LALT	KEY_RALT



// DOOM basic types (boolean),
//  and max/min values.
//#include "doomtype.h"

// Fixed point.
//#include "m_fixed.h"

// Endianess handling.
//#include "m_swap.h"


// Binary Angles, sine/cosine/atan lookups.
//#include "tables.h"

// Event type.
//#include "d_event.h"

// Game function, skills.
//#include "g_game.h"

// All external data is defined here.
//#include "doomdata.h"

// All important printed strings.
// Language selection (message strings).
//#include "dstrings.h"

// Player is a special actor.
//struct player_s;


//#include "d_items.h"
//#include "d_player.h"
//#include "p_mobj.h"
//#include "d_net.h"

// PLAY
//#include "p_tick.h"




// Header, generated by sound utility.
// The utility was written by Dave Taylor.
//#include "sounds.h"




#endif          // __DOOMDEF__
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
