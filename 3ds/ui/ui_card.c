// The trainer card, drawn from what the link already exchanged. See ui_card.h.
//
// The card is two GBA background layers composited, exactly as
// SetCardBgsAndPals does (src/trainer_card.c): a background tilemap in palette
// 1, and the card itself on top in palette 0, which keys on index 0 so the
// background shows through. Both maps flip tiles, so every entry goes through
// UiBlit4bppTileFlip.
//
// The star-tier palettes are file-local to src/trainer_card.c, so they are
// included again here from the same source files rather than reached for. The
// 3ds/ui sources go through the same asset preprocessor as src/ (build_objs.sh),
// so INCGFX works here.

#include "global.h"
#include "link.h"                     // gLinkPlayers, gReceivedRemoteLinkPlayers
#include "trainer_card.h"             // gTrainerCards
#include "easy_chat.h"                // CopyEasyChatWord
#include "decompress.h"               // LZDecompressWram, GetDecompressedDataSize
#include "graphics.h"
#include "string_util.h"
#include "strings.h"
#include "constants/characters.h"           // EOS
#include "constants/trainers.h"

#include "../bridge.h"
#include "ui_draw.h"
#include "ui_text.h"
#include "ui_shell.h"
#include "ui_card.h"

// The card is 30x20 tiles. Its tile sheet is 192 tiles, which is what
// SetCardBgsAndPals loads (0x1800 bytes).
#define CARD_TW     (UI_CARD_W / 8)
#define CARD_TH     (UI_CARD_H / 8)
#define CARD_TILES  192
#define CARD_ENTRIES (CARD_TW * CARD_TH)

// A GBA tilemap entry.
#define MAP_TILE(e)  ((e) & 0x3FF)
#define MAP_HFLIP(e) ((e) & 0x0400)
#define MAP_VFLIP(e) ((e) & 0x0800)

// The star row: tile 143 of the sheet, at tile (15, 7), one tile per star, in
// its own palette. From DrawStarsAndBadgesOnCard (src/trainer_card.c).
#define STAR_TILE   143
#define STAR_TX     15
#define STAR_TY     7

// The card's text window sits at tile (1, 1), so every coordinate taken from
// src/trainer_card.c is relative to this. See sTrainerCardWindowTemplates.
#define TEXT_X0     8
#define TEXT_Y0     8

// The trainer's own picture. Its window is at tile (19, 5) and the pic sits one
// pixel into it, from sTrainerCardWindowTemplates and sTrainerPicOffset. It is
// 64x64 in a 72x80 window, so it is the window that has the slack, not the
// card.
#define PIC_X       (19 * 8 + 1)
#define PIC_Y       (5 * 8)

// Hoenn front, from PrintNameOnCardFront, PrintIdOnCard, PrintMoneyOnCard,
// PrintPokedexOnCard and PrintTimeOnCard. The value columns are right-aligned
// edges, not left origins.
#define FRONT_LABEL_X   16
#define FRONT_VALUE_R   128
#define FRONT_NAME_Y    33
#define FRONT_ID_Y      9
#define FRONT_MONEY_Y   57
#define FRONT_DEX_Y     73
#define FRONT_TIME_Y    89

// Back, from PrintNameOnCardBack and PrintStatOnBackOfCard.
#define BACK_LABEL_X    10
#define BACK_VALUE_R    216
#define BACK_NAME_Y     9
#define BACK_ROW0_Y     33
#define BACK_ROW_H      16
#define BACK_PHRASE_Y   120

// The star-tier palettes, from the same files src/trainer_card.c uses. Each is
// three 16-colour palettes: the card takes the first and the background the
// second. gHoennTrainerCardGreen_Pal is the no-star one and is already public.
static const u16 sBronze_Pal[]   = INCGFX_U16("graphics/trainer_card/bronze.pal", ".gbapal");
static const u16 sCopper_Pal[]   = INCGFX_U16("graphics/trainer_card/copper.pal", ".gbapal");
static const u16 sSilver_Pal[]   = INCGFX_U16("graphics/trainer_card/silver.pal", ".gbapal");
static const u16 sGold_Pal[]     = INCGFX_U16("graphics/trainer_card/gold.pal", ".gbapal");
static const u16 sFemaleBg_Pal[] = INCGFX_U16("graphics/trainer_card/female_bg.pal", ".gbapal");
static const u16 sStar_Pal[]     = INCGFX_U16("graphics/trainer_card/star.pal", ".gbapal");

// Indexed by star count. The game's own table has five entries and indexes it
// by `stars` directly, which reads past the end at five stars: a link card
// gains one for all frontier symbols on top of the four this table covers.
// Clamp instead.
static const u16 *const sCardPals[] =
{
    gHoennTrainerCardGreen_Pal,   // 0 stars
    sBronze_Pal,                  // 1
    sCopper_Pal,                  // 2
    sSilver_Pal,                  // 3
    sGold_Pal,                    // 4
};

// ---- the decompressed art, loaded once ------------------------------------

static u8  sTiles[CARD_TILES * 32];
static u16 sMapBg[CARD_ENTRIES];
static u16 sMapFront[CARD_ENTRIES];
static u16 sMapBack[CARD_ENTRIES];
static int sGfxLoaded;

// The palettes for the card currently being drawn, cached on what decides them.
static u16 sPalCard[16];
static u16 sPalBg[16];
static u16 sPalStar[16];
static int sPalStars = -1;
static int sPalGender = -1;
static int sPalStarLoaded;

// Check every destination before decompressing into it, as UiMonPic does: a
// size mismatch would otherwise write past a static.
static int LoadGfx(void)
{
    if (sGfxLoaded)
        return 1;

    if (GetDecompressedDataSize(gHoennTrainerCard_Gfx) > sizeof(sTiles)
     || GetDecompressedDataSize(gHoennTrainerCardBg_Tilemap) > sizeof(sMapBg)
     || GetDecompressedDataSize(gHoennTrainerCardFrontLink_Tilemap) > sizeof(sMapFront)
     || GetDecompressedDataSize(gHoennTrainerCardBack_Tilemap) > sizeof(sMapBack))
        return 0;

    LZDecompressWram(gHoennTrainerCard_Gfx, sTiles);
    LZDecompressWram(gHoennTrainerCardBg_Tilemap, sMapBg);
    LZDecompressWram(gHoennTrainerCardFrontLink_Tilemap, sMapFront);
    LZDecompressWram(gHoennTrainerCardBack_Tilemap, sMapBack);

    sGfxLoaded = 1;
    return 1;
}

static void LoadPals(int stars, int gender)
{
    if (!sPalStarLoaded)
    {
        UiLoadPal(sPalStar, sStar_Pal, 16);
        sPalStarLoaded = 1;
    }

    if (sPalStars == stars && sPalGender == gender)
        return;

    // Three palettes in one block: the card takes the first, the background the
    // second, and a female trainer replaces the second outright.
    UiLoadPal(sPalCard, sCardPals[stars], 16);
    UiLoadPal(sPalBg, (gender == FEMALE) ? sFemaleBg_Pal : sCardPals[stars] + 16, 16);

    sPalStars = stars;
    sPalGender = gender;
}

// ---- the data ---------------------------------------------------------------

int UiCardAvailable(int cardId)
{
    // gTrainerCards is never cleared, and IsLinkConnectionEstablished and
    // gReceivedRemoteLinkPlayers both go true before the card block arrives, so
    // neither says the card on hand is this partner's. What does: the card
    // carries the low 16 bits of the trainer id and the link player carries all
    // 32, so a mismatch means the card is the last partner's.
    if (cardId < 0 || cardId >= (int)ARRAY_COUNT(gTrainerCards))
        return 0;

    if (!gReceivedRemoteLinkPlayers)
        return 0;

    return gTrainerCards[cardId].trainerId == (u16)gLinkPlayers[cardId].trainerId;
}

// Which trainer the card shows.
//
// Outside the Union Room the GBA card's face is purely a function of the card
// type and the gender: Brendan or May, and their Ruby and Sapphire poses for a
// partner on those games. It is never the player's own sprite, so this is the
// whole of it. See sTrainerPicFacilityClass and CreateTrainerCardTrainerPic
// (src/trainer_card.c); gFacilityClassToPicIndex maps each of these four
// classes straight to the matching TRAINER_PIC_*, so there is no table to
// index and no bound to get wrong.
//
// The union-room class the card also carries is deliberately not used: it would
// show a different trainer from the one the game's own card shows for the same
// player.
static u16 CardTrainerPic(const struct TrainerCard *card)
{
    int rs = (card->version == VERSION_RUBY || card->version == VERSION_SAPPHIRE);

    if (card->gender == FEMALE)
        return rs ? TRAINER_PIC_RS_MAY : TRAINER_PIC_MAY;

    return rs ? TRAINER_PIC_RS_BRENDAN : TRAINER_PIC_BRENDAN;
}

static int CardStars(const struct TrainerCard *card)
{
    int stars = card->stars;

    if (stars < 0)
        stars = 0;
    if (stars > (int)ARRAY_COUNT(sCardPals) - 1)
        stars = (int)ARRAY_COUNT(sCardPals) - 1;

    return stars;
}

// ---- the art ----------------------------------------------------------------

static void BlitMap(int x, int y, const u16 *map, const u16 *pal, int transparent0)
{
    for (int ty = 0; ty < CARD_TH; ty++)
    {
        for (int tx = 0; tx < CARD_TW; tx++)
        {
            u16 e = map[ty * CARD_TW + tx];
            u32 tile = MAP_TILE(e);

            if (tile >= CARD_TILES)
                continue;

            UiBlit4bppTileFlip(x + tx * 8, y + ty * 8, sTiles + tile * 32, pal,
                               transparent0, MAP_HFLIP(e), MAP_VFLIP(e));
        }
    }
}

// One pixel of a tilemap, as a palette index. 0 is transparent.
static u32 TileIndexAt(const u16 *map, int px, int py)
{
    int tx = px / 8, ty = py / 8;
    int ox = px & 7, oy = py & 7;
    u16 e = map[ty * CARD_TW + tx];
    u32 tile = MAP_TILE(e);
    int sx = MAP_HFLIP(e) ? (7 - ox) : ox;
    int sy = MAP_VFLIP(e) ? (7 - oy) : oy;
    const u8 *t;

    if (tile >= CARD_TILES)
        return 0;

    t = sTiles + tile * 32 + sy * 4;
    return (sx & 1) ? (t[sx >> 1] >> 4) : (t[sx >> 1] & 0xF);
}

// One composited card pixel, for the thumbnail: the card where it is opaque,
// the background underneath where it is not. The same order BlitMap draws in.
static u16 CardPixel(const u16 *map, int px, int py)
{
    u32 idx = TileIndexAt(map, px, py);

    if (idx != 0)
        return sPalCard[idx];

    return sPalBg[TileIndexAt(sMapBg, px, py)];
}

// ---- the text ---------------------------------------------------------------

// The peer's name is text another player typed, and this UI has no clipping, so
// measure it and stop before the value column.
static void DrawNameClipped(int x, int y, const u8 *name, int limit, u16 fg)
{
    u8 buf[PLAYER_NAME_LENGTH + 1];
    int n = 0;

    while (name[n] != EOS && n < PLAYER_NAME_LENGTH)
        n++;

    memcpy(buf, name, (size_t)n);
    buf[n] = EOS;

    while (n > 0 && UiTextWidth(buf) > limit)
        buf[--n] = EOS;

    UiText(x, y, buf, fg, UiThemeShadow());
}

static void DrawFront(int x, int y, int cardId, const struct TrainerCard *card)
{
    u8 buf[32];
    u16 fg = UI_COL_SHADOW;
    int lx = x + TEXT_X0 + FRONT_LABEL_X;
    int rx = x + TEXT_X0 + FRONT_VALUE_R;

    // NAME/ and the trainer's own name. Read it from gLinkPlayers, which the
    // link layer already put through ConvertInternationalString; the copy in
    // the card did not go through it.
    {
        int w = UiText(lx, y + TEXT_Y0 + FRONT_NAME_Y,
                       gText_TrainerCardName, fg, UiThemeShadow());

        DrawNameClipped(lx + w, y + TEXT_Y0 + FRONT_NAME_Y,
                        gLinkPlayers[cardId].name, rx - lx - w, fg);
    }

    // IDNo., five digits with leading zeros, centred over the right column.
    {
        u8 *p = StringCopy(buf, gText_TrainerCardIDNo);

        ConvertIntToDecimalStringN(p, card->trainerId, STR_CONV_MODE_LEADING_ZEROS, 5);
        UiText(x + TEXT_X0 + 120 + (96 - UiTextWidth(buf)) / 2,
               y + TEXT_Y0 + FRONT_ID_Y, buf, fg, UiThemeShadow());
    }

    UiText(lx, y + TEXT_Y0 + FRONT_MONEY_Y, gText_TrainerCardMoney, fg, UiThemeShadow());
    UiNumRight(rx, y + TEXT_Y0 + FRONT_MONEY_Y, (s32)card->money, fg, UiThemeShadow());

    UiText(lx, y + TEXT_Y0 + FRONT_DEX_Y, gText_TrainerCardPokedex, fg, UiThemeShadow());
    if (card->hasPokedex)
        UiNumRight(rx, y + TEXT_Y0 + FRONT_DEX_Y, card->caughtMonsCount, fg, UiThemeShadow());

    UiText(lx, y + TEXT_Y0 + FRONT_TIME_Y, gText_TrainerCardTime, fg, UiThemeShadow());
    {
        int vx = rx;

        vx -= UiNumWidth(card->playTimeMinutes / 10) + UiNumWidth(card->playTimeMinutes % 10);
        UiNum(vx, y + TEXT_Y0 + FRONT_TIME_Y, card->playTimeMinutes / 10, fg, UiThemeShadow());
        UiNum(vx + UiNumWidth(card->playTimeMinutes / 10), y + TEXT_Y0 + FRONT_TIME_Y,
              card->playTimeMinutes % 10, fg, UiThemeShadow());

        vx -= UiTextWidth(UiAscii(buf, ":", sizeof(buf)));
        UiText(vx, y + TEXT_Y0 + FRONT_TIME_Y, buf, fg, UiThemeShadow());

        vx -= UiNumWidth(card->playTimeHours);
        UiNum(vx, y + TEXT_Y0 + FRONT_TIME_Y, card->playTimeHours, fg, UiThemeShadow());
    }
}

// One "label ...... value" row of the back, or "---" when the sender's game did
// not carry that field. A Ruby or Sapphire partner only has the first 0x38
// bytes copied (CopyTrainerCardData), so most of this side is zero for them.
static void BackRow(int x, int y, int row, const char *label, s32 value, int known)
{
    u8 buf[24];
    u16 fg = UI_COL_SHADOW;
    int ry = y + TEXT_Y0 + BACK_ROW0_Y + row * BACK_ROW_H;

    UiText(x + TEXT_X0 + BACK_LABEL_X, ry,
           UiAscii(buf, label, sizeof(buf)), fg, UiThemeShadow());

    if (known)
        UiNumRight(x + TEXT_X0 + BACK_VALUE_R, ry, value, fg, UiThemeShadow());
    else
        UiTextRight(x + TEXT_X0 + BACK_VALUE_R, ry,
                    UiAscii(buf, "---", sizeof(buf)), UI_COL_DIM, UiThemeShadow());
}

static void DrawBack(int x, int y, int cardId, const struct TrainerCard *card)
{
    u8 buf[32];
    u16 fg = UI_COL_SHADOW;
    // Ruby and Sapphire stop at 0x38, so everything past playTime is theirs
    // only if they sent it.
    int full = (card->version != VERSION_RUBY && card->version != VERSION_SAPPHIRE);

    DrawNameClipped(x + TEXT_X0 + BACK_LABEL_X, y + TEXT_Y0 + BACK_NAME_Y,
                    gLinkPlayers[cardId].name, 200, fg);

    BackRow(x, y, 0, "POKEMON TRADES", card->pokemonTrades, full);
    BackRow(x, y, 1, "LINK BATTLES WON", card->linkBattleWins, full);
    BackRow(x, y, 2, "LINK BATTLES LOST", card->linkBattleLosses, full);
    BackRow(x, y, 3, "CONTESTS WITH FRIENDS", card->contestsWithFriends, full);
    BackRow(x, y, 4, "POKEBLOCKS WITH FRIENDS", card->pokeblocksWithFriends, full);

    // The four-word profile the player wrote, which is the only part of the
    // card that is theirs rather than a statistic.
    {
        int px = x + TEXT_X0 + BACK_LABEL_X;

        for (int i = 0; i < TRAINER_CARD_PROFILE_LENGTH; i++)
        {
            int ly = y + TEXT_Y0 + BACK_PHRASE_Y + (i / 2) * UI_LINE_H;

            if (i % 2 == 0)
                px = x + TEXT_X0 + BACK_LABEL_X;

            CopyEasyChatWord(buf, card->easyChatProfile[i]);
            px += UiText(px, ly, buf, fg, UiThemeShadow());
            px += UiTextWidth(UiAscii(buf, " ", sizeof(buf)));
        }
    }
}

// ---- entry points -----------------------------------------------------------

void UiCardDraw(int x, int y, int cardId, int back)
{
    const struct TrainerCard *card;
    int stars;

    if (cardId < 0 || cardId >= (int)ARRAY_COUNT(gTrainerCards) || !LoadGfx())
        return;

    card = &gTrainerCards[cardId];
    stars = CardStars(card);
    LoadPals(stars, card->gender);

    BlitMap(x, y, sMapBg, sPalBg, FALSE);
    BlitMap(x, y, back ? sMapBack : sMapFront, sPalCard, TRUE);

    if (!back)
    {
        // Stars first, then the picture over them. They meet: the star row runs
        // from tile 15 and the picture's window starts at tile 19, so a fifth
        // star lands in the window's first column. On hardware the window's
        // content is what sits on top there, and the picture keys on index 0,
        // so the star still shows through its transparent corner. Five stars
        // needs every frontier symbol, which is why this is rarely seen at all.
        for (int i = 0; i < stars; i++)
            UiBlit4bppTile(x + (STAR_TX + i) * 8, y + STAR_TY * 8,
                           sTiles + STAR_TILE * 32, sPalStar, TRUE);

        UiTrainerPic(x + PIC_X, y + PIC_Y, CardTrainerPic(card));

        DrawFront(x, y, cardId, card);
    }
    else
    {
        DrawBack(x, y, cardId, card);
    }
}

void UiCardThumb(int x, int y, int cardId)
{
    const struct TrainerCard *card;

    if (cardId < 0 || cardId >= (int)ARRAY_COUNT(gTrainerCards) || !LoadGfx())
        return;

    card = &gTrainerCards[cardId];
    LoadPals(CardStars(card), card->gender);

    // Every fourth pixel, a row at a time. The art stays recognisable and
    // nothing tries to be legible at this size.
    for (int ty = 0; ty < UI_CARD_THUMB_H; ty++)
    {
        u16 row[UI_CARD_THUMB_W];

        for (int tx = 0; tx < UI_CARD_THUMB_W; tx++)
            row[tx] = CardPixel(sMapFront, tx * 4, ty * 4);

        UiBlitRow(x, y + ty, row, UI_CARD_THUMB_W);
    }
}
