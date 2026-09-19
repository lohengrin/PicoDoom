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
//	Refresh, visplane stuff (floor, ceilings).
//
//-----------------------------------------------------------------------------


#ifndef __R_PLANE__
#define __R_PLANE__


#include "r_data.h"

#ifdef __GNUG__
#pragma interface
#endif


// Visplane related.
extern  short*		lastopening;

#ifdef PICO
// MAXOPENINGS/openings[] moved here (from a local #define + file-scope
// array in r_plane.c) so r_segs.c can bounds-check writes through
// lastopening before they happen -- see r_segs.c's #ifdef PICO comments
// for why: the only bounds check in the original source (r_plane.c,
// R_DrawPlanes) runs *after* a frame's walls are already rendered, well
// after r_segs.c's own lastopening writes/advances could already have
// run past the end of openings[]. A silent out-of-bounds write into
// whatever .bss data happens to follow openings[] -- not a crash, not an
// error, just quiet corruption of something else entirely -- confirmed as
// the cause of a real, reproducible hang on this hardware (see
// docs/PLAN.md): a level area revealing a lot of new wall geometry at
// once (a secret door opening) is exactly the scenario that pushes
// lastopening furthest in one frame.
#define MAXOPENINGS (MAX_SCREENWIDTH*64)
extern short openings[MAXOPENINGS];
#endif


typedef void (*planefunction_t) (int top, int bottom);

extern planefunction_t	floorfunc;
extern planefunction_t	ceilingfunc_t;

extern short		floorclip[MAX_SCREENWIDTH];
extern short		ceilingclip[MAX_SCREENWIDTH];

extern fixed_t		yslope[MAX_SCREENHEIGHT];
extern fixed_t		distscale[MAX_SCREENWIDTH];

void R_InitPlanes (void);
void R_ClearPlanes (void);

void
R_MapPlane
( int		y,
  int		x1,
  int		x2 );

void
R_MakeSpans
( int		x,
  int		t1,
  int		b1,
  int		t2,
  int		b2 );

void R_DrawPlanes (void);

visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel );

visplane_t*
R_CheckPlane
( visplane_t*	pl,
  int		start,
  int		stop );



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
