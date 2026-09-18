// LINK: pairing for the Cable Club over 3DS local wireless. Page 5 of the
// EXTRA tab. See ui_link.h for why it is a page and not a tab.
//
// A cable decides the master by which end you plug into. Over wireless someone
// has to create the network and the other has to find it, so that choice is
// made here explicitly rather than by an automatic scan-then-host, which races:
// if both consoles scan at the same moment, both host and neither sees the
// other.
//
// This panel only pairs. Once connected, everything else is the game's own
// Cable Club: walk to the counter in the Pokemon Center and it behaves exactly
// as it does over a cable, because src/link.c's queues are all that changed.
//
// Cross-play with a real GBA is impossible, and the wording here must never
// suggest otherwise.

#include "global.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_link.h"

#define BTN_H        30
#define HOST_X       16
#define HOST_W       130
#define SCAN_X       162
#define SCAN_W       130
#define BTN_Y        30

#define LIST_Y       74
#define ROW_H        26
#define VISIBLE_ROWS 4
#define ROW_X        16
#define ROW_W        288

#define STOP_X       16
#define STOP_W       130
#define STOP_Y       150

static void DrawButton(int x, int y, int w, const char *text, u16 fg)
{
    u8 label[24];

    UiRect(x, y, w, BTN_H, UI_COL_DIM);
    UiAscii(label, text, sizeof(label));
    UiText(x + (w - UiTextWidth(label)) / 2, y + (BTN_H - UI_GLYPH_H) / 2,
           label, fg, UiThemeShadow());
}

// The host name comes from the other console, not from Emerald, so it is plain
// ASCII rather than the game's encoding. UiAscii is the right conversion.
static void DrawScanList(void)
{
    u8 label[40];
    int n = Ctr3dsLinkScanCount();

    if (n == 0)
    {
        UiText(ROW_X, LIST_Y, UiAscii(label, "No games found. Tap SCAN.", sizeof(label)),
               UI_COL_DIM, UiThemeShadow());
        return;
    }

    for (int i = 0; i < n && i < VISIBLE_ROWS; i++)
    {
        int y = LIST_Y + i * ROW_H;
        char name[CTR_LINK_NAME_LEN];

        UiRect(ROW_X, y - 3, ROW_W, ROW_H - 2, UI_COL_DIM);

        Ctr3dsLinkScanName(i, name, sizeof(name));
        UiText(ROW_X + 8, y, UiAscii(label, name, sizeof(label)),
               UiThemeText(), UiThemeShadow());

        UiNumRight(ROW_X + ROW_W - 10, y, Ctr3dsLinkScanPlayers(i),
                   UI_COL_DIM, UiThemeShadow());
    }
}

static void DrawConnected(const CtrLinkStatus *st)
{
    u8 label[40];

    UiText(ROW_X, LIST_Y,
           UiAscii(label, st->isHost ? "Hosting." : "Connected.", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    UiText(ROW_X, LIST_Y + 24, UiAscii(label, "Players", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiNum(ROW_X + 70, LIST_Y + 24, st->playerCount, UiThemeText(), UiThemeShadow());

    UiText(ROW_X, LIST_Y + 48,
           UiAscii(label, st->playerCount > 1 ? "Go to the Cable Club."
                                              : "Waiting for a player...", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    DrawButton(STOP_X, STOP_Y, STOP_W, "DISCONNECT", UI_COL_ACCENT);
}

void UiLinkPageDraw(void)
{
    CtrLinkStatus st;
    u8 label[40];

    Ctr3dsLinkGetStatus(&st);

    if (st.state == CTR_LINK_HOSTING || st.state == CTR_LINK_CONNECTED)
    {
        DrawConnected(&st);
        return;
    }

    // A pairing call is running on the worker thread. Say so, and dim the
    // buttons, because they do nothing until it ends. The game keeps running
    // behind this panel, which is the whole point of the worker.
    if (st.state == CTR_LINK_SCANNING || st.state == CTR_LINK_JOINING
        || st.state == CTR_LINK_WORKING)
    {
        const char *what = st.state == CTR_LINK_SCANNING ? "Scanning..."
                         : st.state == CTR_LINK_JOINING  ? "Joining..."
                                                         : "Please wait...";

        DrawButton(HOST_X, BTN_Y, HOST_W, "HOST", UI_COL_DIM);
        DrawButton(SCAN_X, BTN_Y, SCAN_W, "SCAN", UI_COL_DIM);
        UiText(ROW_X, LIST_Y, UiAscii(label, what, sizeof(label)),
               UiThemeText(), UiThemeShadow());
        return;
    }

    DrawButton(HOST_X, BTN_Y, HOST_W, "HOST", UiThemeText());
    DrawButton(SCAN_X, BTN_Y, SCAN_W, "SCAN", UiThemeText());

    if (st.state == CTR_LINK_FAILED)
    {
        UiText(ROW_X, LIST_Y, UiAscii(label, "Wireless failed. Try again.", sizeof(label)),
               UI_COL_HP_LOW, UiThemeShadow());
        return;
    }

    DrawScanList();
}

void UiLinkPageTouch(const CtrTouchState *t)
{
    CtrLinkStatus st;

    if (!t->justReleased)
        return;

    // The worker owns the wireless while a pairing call runs. link.c drops a
    // second request anyway; refusing here keeps the panel honest about it.
    if (Ctr3dsLinkBusy())
        return;

    Ctr3dsLinkGetStatus(&st);

    if (st.state == CTR_LINK_HOSTING || st.state == CTR_LINK_CONNECTED)
    {
        if (UiHit(t, STOP_X, STOP_Y, STOP_W, BTN_H))
        {
            Ctr3dsLinkStop();
            UiMarkDirty();
        }
        return;
    }

    if (UiHit(t, HOST_X, BTN_Y, HOST_W, BTN_H))
    {
        Ctr3dsLinkHost();
        UiMarkDirty();
        return;
    }

    // The sweep runs on the link worker, so this returns at once and the panel
    // shows "Scanning..." until the results land.
    if (UiHit(t, SCAN_X, BTN_Y, SCAN_W, BTN_H))
    {
        Ctr3dsLinkScan();
        UiMarkDirty();
        return;
    }

    for (int i = 0; i < Ctr3dsLinkScanCount() && i < VISIBLE_ROWS; i++)
    {
        if (UiHit(t, ROW_X, LIST_Y + i * ROW_H - 3, ROW_W, ROW_H - 2))
        {
            Ctr3dsLinkJoin(i);
            UiMarkDirty();
            return;
        }
    }
}

// Bits 19-21 the state, 22-23 the player count, 28-31 the number of scan
// results. UiExtraStateKey() owns every other bit of the value and calls this
// only while the LINK page shows, so no other page polls the wireless.
u32 UiLinkPageStateKey(void)
{
    CtrLinkStatus st;
    int n = Ctr3dsLinkScanCount();

    Ctr3dsLinkGetStatus(&st);

    if (n > 15)
        n = 15;

    // playerCount is 1..4 when connected, so minus 1 gives 0..3. It is 0 when
    // idle, which wraps to 3, but the state bits above tell those two apart.
    return ((u32)(st.state & 7) << 19)
         | ((u32)((st.playerCount - 1) & 3) << 22)
         | ((u32)n << 28);
}
