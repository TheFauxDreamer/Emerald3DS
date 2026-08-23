// DEX tab. Placeholder until the real entry list lands. It is only reachable
// once FLAG_SYS_POKEDEX_GET is set, so reaching this screen at all means the
// player really does have the Pokedex.
//
// The real version reads GetHoennPokedexCount() / GetNationalPokedexCount() and
// filters by GetSetPokedexFlag(); see 3ds/ROADMAP.md Stage 5.

#include "global.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

void UiDexDraw(void)
{
    u8 label[24];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);
    UiText(16, 16, UiAscii(label, "POKEDEX", sizeof(label)),
           UiThemeText(), UiThemeShadow());
    UiText(16, 40, UiAscii(label, "Coming soon", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
}

void UiDexTouch(const CtrTouchState *t)
{
    (void)t;
}
