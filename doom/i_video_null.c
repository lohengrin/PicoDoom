// Headless video bring-up stub for the Pico port.
// Replaced by the real ILI9486 LCD driver (see TOM6809 Ili9486Display).
#include "i_video.h"
#include "i_system.h"

void I_InitGraphics(void) {}
void I_ShutdownGraphics(void) {}
void I_SetPalette(byte* palette) {}
void I_UpdateNoBlit(void) {}
void I_FinishUpdate(void) {}
void I_ReadScreen(byte* scr) {}
void I_StartTic(void) {}
void I_StartFrame(void) {}