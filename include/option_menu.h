#ifndef GUARD_OPTION_MENU_H
#define GUARD_OPTION_MENU_H

void CB2_InitOptionMenu(void);

#if PLATFORM_3DS
// The window frame that the player sees now, or -1 when the options menu is not
// open. The menu keeps its working copy in its own task, and writes
// gSaveBlock2Ptr only on exit. Thus the second screen needs this value to show
// the frame as the top screen does.
s16 Ctr3dsLiveWindowFrameType(void);
#endif

#endif // GUARD_OPTION_MENU_H
