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
//	Networking stuff.
//
//-----------------------------------------------------------------------------


#ifndef __D_NET__
#define __D_NET__

#include "d_player.h"


#ifdef __GNUG__
#pragma interface
#endif


//
// Network play related stuff.
// There is a data struct that stores network
//  communication related stuff, and another
//  one that defines the actual packets to
//  be transmitted.
//

#define DOOMCOM_ID		0x12345678l

// Max computers/players in a game.
#define MAXNETNODES		8


// Networking and tick handling related.
#define BACKUPTICS		12

typedef enum
{
    CMD_SEND	= 1,
    CMD_GET	= 2

} command_t;


//
// Network packet data. Would be the actual wire format in a multiplayer
// game (see docs/PLAN.md's type-size audit -- currently unreachable code,
// i_net_null.c's doomcom fixes numnodes=1, but pinned to explicit widths
// now rather than if/when that ever changes).
//
typedef struct
{
    // High bit is retransmit request.
    uint32_t		checksum;
    // Only valid if NCMD_RETRANSMIT.
    uint8_t		retransmitfrom;

    uint8_t		starttic;
    uint8_t		player;
    uint8_t		numtics;
    ticcmd_t		cmds[BACKUPTICS];

} doomdata_t;




// This is the struct src/i_net_null.c actually fills in for real (see its
// I_InitNetwork, mirroring i_net.c's own single-player setup path) -- a
// genuine cross-boundary contract for this port, even without real
// networking, so pinned the same as doomdata_t above. `long id` was the
// one field worth specifically flagging: id==4 bytes here (ILP32 either of
// this port's platforms), but `long` is 8 bytes on plenty of real-world
// LP64 targets -- exactly the kind of assumption that's silently fine until
// it isn't.
typedef struct
{
    // Supposed to be DOOMCOM_ID?
    int32_t		id;

    // DOOM executes an int to execute commands.
    int16_t		intnum;
    // Communication between DOOM and the driver.
    // Is CMD_SEND or CMD_GET.
    int16_t		command;
    // Is dest for send, set by get (-1 = no packet).
    int16_t		remotenode;

    // Number of bytes in doomdata to be sent
    int16_t		datalength;

    // Info common to all nodes.
    // Console is allways node 0.
    int16_t		numnodes;
    // Flag: 1 = no duplication, 2-5 = dup for slow nets.
    int16_t		ticdup;
    // Flag: 1 = send a backup tic in every packet.
    int16_t		extratics;
    // Flag: 1 = deathmatch.
    int16_t		deathmatch;
    // Flag: -1 = new game, 0-5 = load savegame
    int16_t		savegame;
    int16_t		episode;	// 1-3
    int16_t		map;		// 1-9
    int16_t		skill;		// 1-5

    // Info specific to this node.
    int16_t		consoleplayer;
    int16_t		numplayers;

    // These are related to the 3-display mode,
    //  in which two drones looking left and right
    //  were used to render two additional views
    //  on two additional computers.
    // Probably not operational anymore.
    // 1 = left, 0 = center, -1 = right
    int16_t		angleoffset;
    // 1 = drone
    int16_t		drone;

    // The packet data to be sent.
    doomdata_t		data;

} doomcom_t;



// Create any new ticcmds and broadcast to other players.
void NetUpdate (void);

// Broadcasts special packets to other players
//  to notify of game exit
void D_QuitNetGame (void);

//? how many ticks to run?
void TryRunTics (void);


#endif

//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------

