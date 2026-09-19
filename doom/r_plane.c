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
//	Here is a core component: drawing the floors and ceilings,
//	 while maintaining a per column clipping list only.
//	Moreover, the sky areas have to be determined.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_plane.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";

#include <stdlib.h>

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"

#ifdef PICO
extern void *psram_malloc (size_t);
#endif


planefunction_t		floorfunc;
planefunction_t		ceilingfunc;

//
// opening
//

// Here comes the obnoxious "visplane".
#define MAXVISPLANES	128
#ifdef PICO
// The visplane pool is the biggest single static in the engine (~83 KB of
// .bss). R_FindPlane/R_CheckPlane only ever touch the handful of planes
// that are on-screen this frame (typically 1-4), so backing it with PSRAM
// is cheap -- upstream kept it on the zone heap too. Frees the ~83 KB that
// lets both LCD blit buffers live in SRAM (src/i_video_ili9486.cpp). All
// uses below are pointer arithmetic / subscripting, identical for a
// pointer against an allocated pool.
visplane_t*		visplanes;
#else
visplane_t		visplanes[MAXVISPLANES];
#endif
visplane_t*		lastvisplane;
visplane_t*		floorplane;
visplane_t*		ceilingplane;

// ?
#ifndef PICO
// Under PICO, both now come from r_plane.h instead -- see its #ifdef PICO
// comment (r_segs.c needs MAXOPENINGS too, to bounds-check before writing
// through lastopening, not just here, after the fact).
#define MAXOPENINGS	(MAX_SCREENWIDTH*64)
#endif
short			openings[MAXOPENINGS];
short*			lastopening;


//
// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
//
short			floorclip[MAX_SCREENWIDTH];
short			ceilingclip[MAX_SCREENWIDTH];

//
// spanstart holds the start of a plane span
// initialized to 0 at start
//
int			spanstart[MAX_SCREENHEIGHT];
int			spanstop[MAX_SCREENHEIGHT];

//
// texture mapping
//
lighttable_t**		planezlight;
fixed_t			planeheight;

fixed_t			yslope[MAX_SCREENHEIGHT];
fixed_t			distscale[MAX_SCREENWIDTH];
fixed_t			basexscale;
fixed_t			baseyscale;

fixed_t			cachedheight[MAX_SCREENHEIGHT];
fixed_t			cacheddistance[MAX_SCREENHEIGHT];
fixed_t			cachedxstep[MAX_SCREENHEIGHT];
fixed_t			cachedystep[MAX_SCREENHEIGHT];



//
// R_InitPlanes
// Only at game startup.
//
void R_InitPlanes (void)
{
#ifdef PICO
  // Pool is in PSRAM (see the visplanes comment above); only ever called
  // from R_Init(). psram_malloc is already usable here: PSRAM is brought
  // up in main() before D_DoomMain().
  if (!visplanes)
    visplanes = (visplane_t *) psram_malloc (MAXVISPLANES * sizeof (*visplanes));
  if (!visplanes)
    I_Error ("R_InitPlanes: visplane pool alloc failed");
  /* Upstream's static visplanes[] array is zero-initialized .bss; a
   * psram_malloc'd pool carries whatever garbage the previous region
   * occupant left. R_NewPlane() only memsets top[] to 0xff -- bottom[]
   * stays as-is for columns a seg never touches, and a stray 0xff (=255)
   * there makes R_MakeSpans() hand R_MapPlane() a row past viewheight
   * (R_MapPlane: 74, 74 at 255). Zero the pool so unwritten columns keep a
   * benign bottom like upstream. */
  memset (visplanes, 0, MAXVISPLANES * sizeof (*visplanes));
#endif
  // Doh!
}


//
// R_MapPlane
//
// Uses global vars:
//  planeheight
//  ds_source
//  basexscale
//  baseyscale
//  viewx
//  viewy
//
// BASIC PRIMITIVE
//
void
R_MapPlane
( int		y,
  int		x1,
  int		x2 )
{
    angle_t	angle;
    fixed_t	distance;
    fixed_t	length;
    unsigned	index;
	
#ifdef RANGECHECK
    if (x2 < x1
	|| x1<0
	|| x2>=viewwidth
	|| (unsigned)y>viewheight)
    {
	I_Error ("R_MapPlane: %i, %i at %i",x1,x2,y);
    }
#endif

    if (planeheight != cachedheight[y])
    {
	cachedheight[y] = planeheight;
	distance = cacheddistance[y] = FixedMul (planeheight, yslope[y]);
	ds_xstep = cachedxstep[y] = FixedMul (distance,basexscale);
	ds_ystep = cachedystep[y] = FixedMul (distance,baseyscale);
    }
    else
    {
	distance = cacheddistance[y];
	ds_xstep = cachedxstep[y];
	ds_ystep = cachedystep[y];
    }
	
    length = FixedMul (distance,distscale[x1]);
    angle = (viewangle + xtoviewangle[x1])>>ANGLETOFINESHIFT;
    ds_xfrac = viewx + FixedMul(finecosine[angle], length);
    ds_yfrac = -viewy - FixedMul(finesine[angle], length);

    if (fixedcolormap)
	ds_colormap = fixedcolormap;
    else
    {
	index = distance >> LIGHTZSHIFT;
	
	if (index >= MAXLIGHTZ )
	    index = MAXLIGHTZ-1;

	ds_colormap = planezlight[index];
    }
	
    ds_y = y;
    ds_x1 = x1;
    ds_x2 = x2;

    // high or low detail
    spanfunc ();	
}


//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes (void)
{
    int		i;
    angle_t	angle;
    
    // opening / clipping determination
    for (i=0 ; i<viewwidth ; i++)
    {
	floorclip[i] = viewheight;
	ceilingclip[i] = -1;
    }

    lastvisplane = visplanes;
    lastopening = openings;
    
    // texture calculation
    memset (cachedheight, 0, sizeof(cachedheight));

    // left to right mapping
    angle = (viewangle-ANG90)>>ANGLETOFINESHIFT;
	
    // scale will be unit scale at SCREENWIDTH/2 distance
    basexscale = FixedDiv (finecosine[angle],centerxfrac);
    baseyscale = -FixedDiv (finesine[angle],centerxfrac);
}




//
// R_FindPlane
//
visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel )
{
    visplane_t*	check;
	
    if (picnum == skyflatnum)
    {
	height = 0;			// all skys map together
	lightlevel = 0;
    }
	
    for (check=visplanes; check<lastvisplane; check++)
    {
	if (height == check->height
	    && picnum == check->picnum
	    && lightlevel == check->lightlevel)
	{
	    break;
	}
    }
    
			
    if (check < lastvisplane)
	return check;
		
    if (lastvisplane - visplanes == MAXVISPLANES)
	I_Error ("R_FindPlane: no more visplanes");
		
    lastvisplane++;

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = SCREENWIDTH;
    check->maxx = -1;
    
    memset (check->top,0xff,sizeof(check->top));
		
    return check;
}


//
// R_CheckPlane
//
visplane_t*
R_CheckPlane
( visplane_t*	pl,
  int		start,
  int		stop )
{
    int		intrl;
    int		intrh;
    int		unionl;
    int		unionh;
    int		x;
	
    if (start < pl->minx)
    {
	intrl = pl->minx;
	unionl = start;
    }
    else
    {
	unionl = pl->minx;
	intrl = start;
    }
	
    if (stop > pl->maxx)
    {
	intrh = pl->maxx;
	unionh = stop;
    }
    else
    {
	unionh = pl->maxx;
	intrh = stop;
    }

    for (x=intrl ; x<= intrh ; x++)
	if (pl->top[x] != 0xff)
	    break;

    if (x > intrh)
    {
	pl->minx = unionl;
	pl->maxx = unionh;

	// use the same one
	return pl;		
    }
	
    // make a new visplane
    lastvisplane->height = pl->height;
    lastvisplane->picnum = pl->picnum;
    lastvisplane->lightlevel = pl->lightlevel;
    
    pl = lastvisplane++;
    pl->minx = start;
    pl->maxx = stop;

    memset (pl->top,0xff,sizeof(pl->top));
		
    return pl;
}


//
// R_MakeSpans
//
void
R_MakeSpans
( int		x,
  int		t1,
  int		b1,
  int		t2,
  int		b2 )
{
    while (t1 < t2 && t1<=b1)
    {
	R_MapPlane (t1,spanstart[t1],x-1);
	t1++;
    }
    while (b1 > b2 && b1>=t1)
    {
	R_MapPlane (b1,spanstart[b1],x-1);
	b1--;
    }
	
    while (t2 < t1 && t2<=b2)
    {
	spanstart[t2] = x;
	t2++;
    }
    while (b2 > b1 && b2>=t2)
    {
	spanstart[b2] = x;
	b2--;
    }
}



//
// R_DrawPlanes
// At the end of each frame.
//
void R_DrawPlanes (void)
{
    visplane_t*		pl;
    int			light;
    int			x;
    int			stop;
    int			angle;
				
#ifdef RANGECHECK
    if (ds_p - drawsegs > MAXDRAWSEGS)
	I_Error ("R_DrawPlanes: drawsegs overflow (%i)",
		 ds_p - drawsegs);
    
    if (lastvisplane - visplanes > MAXVISPLANES)
	I_Error ("R_DrawPlanes: visplane overflow (%i)",
		 lastvisplane - visplanes);
    
    if (lastopening - openings > MAXOPENINGS)
	I_Error ("R_DrawPlanes: opening overflow (%i)",
		 lastopening - openings);
#endif

    for (pl = visplanes ; pl < lastvisplane ; pl++)
    {
	if (pl->minx > pl->maxx)
	    continue;

	
	// sky flat
	if (pl->picnum == skyflatnum)
	{
	    dc_iscale = pspriteiscale>>detailshift;
	    
	    // Sky is allways drawn full bright,
	    //  i.e. colormaps[0] is used.
	    // Because of this hack, sky is not affected
	    //  by INVUL inverse mapping.
	    dc_colormap = colormaps;
	    dc_texturemid = skytexturemid;
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		dc_yl = pl->top[x];
		dc_yh = pl->bottom[x];

		if (dc_yl <= dc_yh)
		{
		    angle = (viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
		    dc_x = x;
		    dc_source = R_GetColumn(skytexture, angle);
		    colfunc ();
		}
	    }
	    continue;
	}
	
	// regular flat
	ds_source = W_CacheLumpNum(firstflat +
				   flattranslation[pl->picnum],
				   PU_STATIC);
	
	planeheight = abs(pl->height-viewz);
	light = (pl->lightlevel >> LIGHTSEGSHIFT)+extralight;

	if (light >= LIGHTLEVELS)
	    light = LIGHTLEVELS-1;

	if (light < 0)
	    light = 0;

	planezlight = zlight[light];

	pl->top[pl->maxx+1] = 0xff;
	pl->top[pl->minx-1] = 0xff;
		
	stop = pl->maxx + 1;

	for (x=pl->minx ; x<= stop ; x++)
	{
	    R_MakeSpans(x,pl->top[x-1],
			pl->bottom[x-1],
			pl->top[x],
			pl->bottom[x]);
	}
	
	Z_ChangeTag (ds_source, PU_CACHE);
    }
}
