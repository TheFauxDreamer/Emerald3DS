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
#include "link.h"                     // IsLinkConnectionEstablished, gReceivedRemoteLinkPlayers

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_card.h"
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

#define CARDS_X      162
#define CARDS_W      130

// ---- the card view ----------------------------------------------------------
//
// Both trainer cards, from the data the link-up already exchanged. A card is a
// whole GBA screen, 240x160, and faithful tile art needs an integer scale, so
// the one being read is drawn at 1:1 and the other is a 1:4 thumbnail beside
// it. That does not fit inside the EXTRA window and its pager, so while this
// view is up the page takes the whole content area and gives the pager back on
// BACK. See UiLinkPageFullBleed.
#define CV_CARD_X    4
#define CV_CARD_Y    16
#define CV_THUMB_X   252
#define CV_THUMB_Y0  16
#define CV_THUMB_DY  62
#define CV_LABEL_DY  (UI_CARD_THUMB_H + 2)
#define CV_BACK_X    252
#define CV_BACK_Y    150
#define CV_BACK_W    60
#define CV_BACK_H    22

static int sCardOpen;
static int sCardWho;     // the player whose card is at 1:1
static int sCardBack;    // the profile side

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

// Is the game itself in a link, rather than merely paired?
//
// Pairing on this panel only puts the two consoles on the same network. The
// session starts when the host confirms at the Cable Club counter, which is
// what moves gLink to LINK_STATE_CONN_ESTABLISHED, and it ends at CloseLink.
// Between those two, DISCONNECT would tear the wireless out from under a trade
// or a battle: the peer sees the link drop and shows Emerald's communication
// error, and a trade caught between the two consoles committing can lose the
// Pokemon. So the button is refused for exactly that window.
//
// Both flags, because they bracket slightly different things and either one
// being set means a session is under way.
static int LinkSessionLive(void)
{
    return IsLinkConnectionEstablished() || gReceivedRemoteLinkPlayers;
}

// Who the two thumbnails are. `slot` 0 is the partner, 1 is this console.
//
// With two players the partner is the other id, which is the game's own idiom
// (gTrainerCards[GetMultiplayerId() ^ 1], src/union_room.c). With more, the
// first id that is not ours stands for them; the view shows one partner, not
// a roster.
static int CardSlotId(int slot)
{
    int local = Ctr3dsLinkLocalId();

    if (slot != 0)
        return local;

    for (int i = 0; i < CTR_LINK_MAX_PLAYERS; i++)
        if (i != local)
            return i;

    return local;
}

// Is there a pair of cards to look at? The partner's is the one that can be
// missing: it arrives at the end of the link-up, after the link is already up.
static int CardsReady(void)
{
    return UiCardAvailable(CardSlotId(0)) && UiCardAvailable(CardSlotId(1));
}

static void DrawThumb(int slot, int y)
{
    int id = CardSlotId(slot);
    u8 label[24];
    int active = (id == sCardWho);

    UiCardThumb(CV_THUMB_X, y, id);
    UiRect(CV_THUMB_X - 1, y - 1, UI_CARD_THUMB_W + 2, UI_CARD_THUMB_H + 2,
           active ? UI_COL_ACCENT : UI_COL_DIM);

    UiAscii(label, (slot == 0) ? "THEM" : "YOU", sizeof(label));
    UiTextSmall(CV_THUMB_X + (UI_CARD_THUMB_W - UiTextSmallWidth(label)) / 2,
                y + CV_LABEL_DY, label,
                active ? UI_COL_ACCENT : UI_COL_DIM, UiThemeShadow());
}

static void DrawCardView(void)
{
    u8 label[24];

    // No window frame: the card is the panel, and it is the size of a whole
    // GBA screen.
    UiClear(UI_COL_BG);

    UiCardDraw(CV_CARD_X, CV_CARD_Y, sCardWho, sCardBack);

    DrawThumb(0, CV_THUMB_Y0);
    DrawThumb(1, CV_THUMB_Y0 + CV_THUMB_DY);

    UiRect(CV_BACK_X, CV_BACK_Y, CV_BACK_W, CV_BACK_H, UI_COL_DIM);
    UiAscii(label, "BACK", sizeof(label));
    UiText(CV_BACK_X + (CV_BACK_W - UiTextWidth(label)) / 2,
           CV_BACK_Y + (CV_BACK_H - UI_GLYPH_H) / 2,
           label, UiThemeText(), UiThemeShadow());

    UiAscii(label, "tap the card to turn it", sizeof(label));
    UiTextSmall(CV_THUMB_X - 4 - UiTextSmallWidth(label), CV_BACK_Y + 4, label,
                UI_COL_DIM, UiThemeShadow());
}

static int TouchCardView(const CtrTouchState *t)
{
    if (UiHit(t, CV_BACK_X, CV_BACK_Y, CV_BACK_W, CV_BACK_H))
    {
        sCardOpen = 0;
        UiMarkDirty();
        return 1;
    }

    for (int slot = 0; slot < 2; slot++)
    {
        int y = CV_THUMB_Y0 + slot * CV_THUMB_DY;

        if (UiHit(t, CV_THUMB_X, y, UI_CARD_THUMB_W, UI_CARD_THUMB_H))
        {
            int id = CardSlotId(slot);

            if (id != sCardWho)
            {
                sCardWho = id;
                sCardBack = 0;      // a different card starts at its front
                UiMarkDirty();
            }
            return 1;
        }
    }

    if (UiHit(t, CV_CARD_X, CV_CARD_Y, UI_CARD_W, UI_CARD_H))
    {
        sCardBack = !sCardBack;
        UiMarkDirty();
        return 1;
    }

    return 1;   // the view owns every touch while it is up
}

static void DrawConnected(const CtrLinkStatus *st)
{
    u8 label[40];
    int live = LinkSessionLive();

    UiText(ROW_X, LIST_Y,
           UiAscii(label, st->isHost ? "Hosting." : "Connected.", sizeof(label)),
           UiThemeText(), UiThemeShadow());

    UiText(ROW_X, LIST_Y + 24, UiAscii(label, "Players", sizeof(label)),
           UI_COL_DIM, UiThemeShadow());
    UiNum(ROW_X + 70, LIST_Y + 24, st->playerCount, UiThemeText(), UiThemeShadow());

    UiText(ROW_X, LIST_Y + 48,
           UiAscii(label,
                   live                   ? "In a link. Finish it first."
                   : st->playerCount > 1  ? "Go to the Cable Club."
                                          : "Waiting for a player...",
                   sizeof(label)),
           UI_COL_DIM, UiThemeShadow());

    // Dimmed, not hidden. A button that vanishes leaves the player looking for
    // it; one that is visibly refused, with the line above saying why, does not.
    DrawButton(STOP_X, STOP_Y, STOP_W, "DISCONNECT",
               live ? UI_COL_DIM : UI_COL_ACCENT);

    // The cards arrive at the end of the link-up, so this is dim until they do.
    DrawButton(CARDS_X, STOP_Y, CARDS_W, "TRAINER CARDS",
               CardsReady() ? UiThemeText() : UI_COL_DIM);
}

// While the card view is up the page needs the whole content area: a card is
// 240x160 and the window frame plus the pager do not leave room for it.
// UiExtraDraw asks before it draws either.
int UiLinkPageFullBleed(void)
{
    return sCardOpen;
}

void UiLinkPageDraw(void)
{
    CtrLinkStatus st;
    u8 label[40];

    // It closes itself if the link goes away under it, so a dropped peer
    // cannot leave a card on screen with no link behind it.
    if (sCardOpen && !CardsReady())
        sCardOpen = 0;

    if (sCardOpen)
    {
        DrawCardView();
        return;
    }

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

    // The card view owns the screen while it is up, including the area the
    // pager would be in.
    if (sCardOpen)
    {
        TouchCardView(t);
        return;
    }

    // The worker owns the wireless while a pairing call runs. link.c drops a
    // second request anyway; refusing here keeps the panel honest about it.
    if (Ctr3dsLinkBusy())
        return;

    Ctr3dsLinkGetStatus(&st);

    if (st.state == CTR_LINK_HOSTING || st.state == CTR_LINK_CONNECTED)
    {
        // Not while the game is in a link. The panel dims the button and says
        // why; this is what makes the refusal real.
        if (LinkSessionLive())
            return;

        if (UiHit(t, CARDS_X, STOP_Y, CARDS_W, BTN_H) && CardsReady())
        {
            sCardOpen = 1;
            sCardWho = CardSlotId(0);   // the other player first
            sCardBack = 0;
            UiMarkDirty();
            return;
        }

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

// Bits 19-21 the state, or 7 for "the game is in a link"; 22-23 the player
// count; 28-31 the number of scan
// results. UiExtraStateKey() owns every other bit of the value and calls this
// only while the LINK page shows, so no other page polls the wireless.
u32 UiLinkPageStateKey(void)
{
    CtrLinkStatus st;
    int n = Ctr3dsLinkScanCount();

    Ctr3dsLinkGetStatus(&st);

    if (n > 15)
        n = 15;

    // Whether the game is in a link belongs in this key too, or the panel would
    // not repaint when a trade starts and the button has to dim. There is no
    // spare bit in this key's allocation (see UiExtraStateKey), and CTR_LINK_*
    // stops at 6, so 7 is free to say it. Which of HOSTING and CONNECTED it was
    // does not matter then: the panel draws the same thing either way.
    u32 state = LinkSessionLive() ? 7u : (u32)(st.state & 7);

    // playerCount is 1..4 when connected, so minus 1 gives 0..3. It is 0 when
    // idle, which wraps to 3, but the state bits above tell those two apart.
    // The top four bits are the scan count while there is a list to scan, and
    // the card view once there is not. State 7 says which: a console in a link
    // is not scanning, so the two can never both be meaningful, and the panel
    // would otherwise not repaint on a card change.
    if (state == 7)
        n = (sCardOpen << 3) | ((sCardWho & 3) << 1) | (sCardBack ? 1 : 0);

    return (state << 19)
         | ((u32)((st.playerCount - 1) & 3) << 22)
         | ((u32)n << 28);
}
