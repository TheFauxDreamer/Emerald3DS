// MAP tab. Placeholder until Stage 4 -- the location readout comes from
// GetMapNameGeneric() and GetCurrentRegionMapSectionId().

#include "global.h"
#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"

void UiMapDraw(void)
{
    u8 label[24];

    UiWindowFrame(0, 0, CTR_BOTTOM_WIDTH / 8, UI_CONTENT_H / 8);
    UiText(16, 16, UiAscii(label, "MAP", sizeof(label)),
           UiThemeText(), UiThemeShadow());
    UiText(16, 40, UiAscii(label, "Coming soon", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
}

void UiMapTouch(const CtrTouchState *t)
{
    (void)t;
}
