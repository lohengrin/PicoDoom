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
//	Fixed point arithemtics, implementation.
//
//-----------------------------------------------------------------------------


#ifndef __M_FIXED__
#define __M_FIXED__


#ifdef __GNUG__
#pragma interface
#endif


//
// Fixed point, 32bit as 16.16.
//
#define FRACBITS		16
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

// Inlined here (not in m_fixed.c, where this used to live as an ordinary
// function) -- Cortex-M33 does the actual 64-bit multiply in one `smull`
// instruction, so the real cost of calling this as a separate function was
// almost entirely branch-and-link/register-save overhead around that one
// instruction. Confirmed hot on real hardware (2026-09 performance work,
// tools/profile_sample.py's SWD sampling profiler: ~3% of all sampled
// program-counter snapshots landed inside a standalone FixedMul() call,
// across the huge number of texture-scale/lighting calculations the
// renderer makes). `static __inline__`, not plain `inline`: this file
// group builds as gnu90 (see CMakeLists.txt), where C89/C99/GNU89 inline
// linkage rules differ in subtle, easy-to-get-wrong ways -- `static`
// sidesteps all of that by giving every translation unit its own
// internal-linkage copy, which -O3 then inlines away at each call site.
static __inline__ fixed_t FixedMul (fixed_t a, fixed_t b)
{
    return (fixed_t)(((long long) a * (long long) b) >> FRACBITS);
}

fixed_t FixedDiv	(fixed_t a, fixed_t b);
fixed_t FixedDiv2	(fixed_t a, fixed_t b);



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
