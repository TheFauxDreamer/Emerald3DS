#ifndef GUARD_TITLE_SCREEN_H
#define GUARD_TITLE_SCREEN_H

extern const u16 gTitleScreenAlphaBlend[64];

void CB2_InitTitleScreen(void);

#if PLATFORM_3DS
// The bottom screen's TOUCH TO START (3ds/ui/ui_title.c), which mirrors the
// PRESS START banner and answers a tap anywhere as a press of START.
enum
{
    CTR3DS_TITLE_PROMPT_NONE,   // not on the title, or PRESS START not up yet
    CTR3DS_TITLE_PROMPT_LIT,    // the banner is showing
    CTR3DS_TITLE_PROMPT_DARK,   // the banner is in the off half of its blink
};

u8 Ctr3dsTitlePromptState(void);
void Ctr3dsTitleTouchStart(void);
#endif

#endif // GUARD_TITLE_SCREEN_H
