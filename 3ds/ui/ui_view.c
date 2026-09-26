// The stack of views over the active tab. See ui_view.h.
//
// No heap, like the rest of 3ds/ui: a few bytes of .bss.

#include "global.h"

#include "ui_shell.h"               // UiMarkDirty
#include "ui_view.h"

struct UiViewEntry
{
    u8  id;
    u16 arg;
};

static struct UiViewEntry sStack[UI_VIEW_DEPTH];
static u8 sDepth;

void UiViewPush(u8 id, u16 arg)
{
    if (id == UI_VIEW_NONE || id >= UI_VIEW_COUNT || sDepth >= UI_VIEW_DEPTH)
        return;

    sStack[sDepth].id = id;
    sStack[sDepth].arg = arg;
    sDepth++;
    UiMarkDirty();
}

void UiViewPop(void)
{
    if (sDepth == 0)
        return;

    sDepth--;
    UiMarkDirty();
}

void UiViewReset(void)
{
    if (sDepth == 0)
        return;

    sDepth = 0;
    UiMarkDirty();
}

bool8 UiViewIsOpen(u8 id)
{
    for (u32 i = 0; i < sDepth; i++)
        if (sStack[i].id == id)
            return TRUE;

    return FALSE;
}

u8 UiViewTop(void)
{
    return sDepth > 0 ? sStack[sDepth - 1].id : UI_VIEW_NONE;
}

u16 UiViewArg(u8 id)
{
    for (u32 i = 0; i < sDepth; i++)
        if (sStack[i].id == id)
            return sStack[i].arg;

    return 0;
}

u32 UiViewKey(void)
{
    u32 key = sDepth;

    for (u32 i = 0; i < sDepth; i++)
        key = key * 2654435761u + (((u32)sStack[i].id << 16) | sStack[i].arg);

    return key;
}
