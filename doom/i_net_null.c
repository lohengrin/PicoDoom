// Headless network bring-up stub for the Pico port.
#include <stdlib.h>
#include <string.h>
#include "i_net.h"
#include "d_net.h"
#include "doomstat.h"

// D_CheckNetGame (d_net.c) unconditionally dereferences doomcom after this
// call and requires doomcom->id == DOOMCOM_ID -- mirror i_net.c's no-"-net"
// (single player) path so a real doomcom_t exists instead of a NULL deref.
void I_InitNetwork(void)
{
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    doomcom->ticdup = 1;
    doomcom->extratics = 0;

    netgame = false;
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
}

void I_NetCmd(void) {}