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
//	Simple basic typedefs, isolated here to make it easier
//	 separating modules.
//    
//-----------------------------------------------------------------------------


#ifndef __DOOMTYPE__
#define __DOOMTYPE__

#include <stdbool.h>
// For the fixed-width types used in cross-boundary structs (ticcmd_t,
// event_t, doomdata_t/doomcom_t -- demo file / would-be network format):
// see docs/PLAN.md's type-size audit. `int`/`short`/`long` are already the
// same size on this port's two platforms, but the explicit-width types
// self-document *why* a field's size is load-bearing, and are what to
// reach for on any genuinely different-width target.
#include <stdint.h>

#ifndef __BYTEBOOL__
#define __BYTEBOOL__
// Fixed to use builtin bool type with C++.
#if defined(__cplusplus) || defined(PICO)
typedef bool boolean;
#else
typedef enum {false, true} boolean;
#endif
typedef unsigned char byte;
#endif


// Predefined with some OS.
#ifdef LINUX
#include <values.h>
#else
#define MAXCHAR		((char)0x7f)
#define MAXSHORT	((short)0x7fff)

// Max pos 32-bit int.
#define MAXINT		((int)0x7fffffff)	
#define MAXLONG		((long)0x7fffffff)
#define MINCHAR		((char)0x80)
#define MINSHORT	((short)0x8000)

// Max negative 32-bit integer.
#define MININT		((int)0x80000000)	
#define MINLONG		((long)0x80000000)
#endif




#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
