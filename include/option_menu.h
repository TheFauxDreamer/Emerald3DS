#ifndef GUARD_OPTION_MENU_H
#define GUARD_OPTION_MENU_H

void CB2_InitOptionMenu(void);

#if PLATFORM_3DS
// The window frame the player is currently previewing, or -1 when the options
// menu is not open. The menu keeps its working copy in its own task and only
// writes gSaveBlock2Ptr on exit, so the second screen needs this to preview
// the border live the way the top screen does.
s16 Ctr3dsLiveWindowFrameType(void);
#endif

#endif // GUARD_OPTION_MENU_H
