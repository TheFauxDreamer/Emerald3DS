#ifndef GUARD_TITLE_SCREEN_H
#define GUARD_TITLE_SCREEN_H

extern const u16 gTitleScreenAlphaBlend[64];

void CB2_InitTitleScreen(void);

#if PLATFORM_3DS
// For TOUCH TO START on the bottom screen (3ds/ui/ui_title.c). It shows while
// the PRESS START banner shows, and a tap anywhere is a press of START.
//
// Returns the frame count of the banner, which is never negative while the
// banner shows. Returns -1 when it does not show: off the title screen, or
// before PRESS START shows.
s32 Ctr3dsTitlePromptClock(void);
void Ctr3dsTitleTouchStart(void);
#endif

#endif // GUARD_TITLE_SCREEN_H
