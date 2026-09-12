#ifndef GUARD_TITLE_SCREEN_H
#define GUARD_TITLE_SCREEN_H

extern const u16 gTitleScreenAlphaBlend[64];

void CB2_InitTitleScreen(void);

#if PLATFORM_3DS
// For the bottom screen's TOUCH TO START (3ds/ui/ui_title.c), which is up while
// the PRESS START banner is and answers a tap anywhere as a press of START.
//
// The banner's own frame count, never negative while it is up, or -1 when it
// is not: off the title, or before PRESS START has appeared.
s32 Ctr3dsTitlePromptClock(void);
void Ctr3dsTitleTouchStart(void);
#endif

#endif // GUARD_TITLE_SCREEN_H
