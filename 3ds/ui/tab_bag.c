// BAG tab. Placeholder until Stage 3 -- listing comes from the game's own
// BagGetItemIdByPocketPosition()/BagGetQuantityByPocketPosition(), and item use
// goes through PokemonUseItemEffects() behind an overworld-only safety gate.

#include "global.h"
#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

void UiBagDraw(void)
{
    u8 label[24];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);
    UiText(16, 16, UiAscii(label, "BAG", sizeof(label)),
           UI_COL_TEXT, UI_COL_SHADOW);
    UiText(16, 40, UiAscii(label, "Coming soon", sizeof(label)),
           UI_COL_DIM, UI_COL_SHADOW);
}

void UiBagTouch(const CtrTouchState *t)
{
    (void)t;
}
