// Bottom-screen UI (game side).
//
// This translation unit is compiled with the game's headers, NOT libctru's --
// see 3ds/bridge.h for why. That is the whole point of the split: Emerald's
// party data, item tables, fonts, mon icons and palettes are ordinary symbols
// here, so the UI reads real game state directly instead of scraping RAM.
//
// PHASE 1 STUB. This draws only a live party/HP readout with plain rectangles,
// which is enough to prove the data path and the bottom-screen upload end to
// end before any font or icon work exists. Phase 3 replaces the body with the
// real tabbed PARTY / BAG / DEX interface drawn in Emerald's own style.

#include "global.h"
#include "pokemon.h"
#include "constants/species.h"

#include "../bridge.h"

#define W CTR_BOTTOM_WIDTH
#define H CTR_BOTTOM_HEIGHT

#define RGB565(r, g, b) \
    (u16)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define COL_BG      RGB565(24, 28, 40)
#define COL_PANEL   RGB565(44, 52, 72)
#define COL_TEXT    RGB565(232, 236, 244)
#define COL_HP_HIGH RGB565(64, 200, 96)
#define COL_HP_MID  RGB565(232, 196, 64)
#define COL_HP_LOW  RGB565(224, 72, 72)
#define COL_HP_BACK RGB565(16, 18, 26)

static u16 sFramebuffer[W * H];
static int sDirty = 1;

// Redrawing 320x240 every frame is pointless for a mostly-static UI, so the
// party state that the display depends on is hashed and compared instead.
static u32 sLastStateHash;

static void FillRect(int x, int y, int w, int h, u16 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        u16 *dst = &sFramebuffer[(y + row) * W + x];
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
}

static u32 PartyStateHash(void)
{
    u32 hash = 2166136261u;   // FNV-1a

    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        struct Pokemon *mon = &gPlayerParty[i];
        u32 fields[4];

        fields[0] = GetMonData(mon, MON_DATA_SPECIES);
        fields[1] = GetMonData(mon, MON_DATA_HP);
        fields[2] = GetMonData(mon, MON_DATA_MAX_HP);
        fields[3] = GetMonData(mon, MON_DATA_LEVEL);

        for (u32 f = 0; f < ARRAY_COUNT(fields); f++)
        {
            hash ^= fields[f];
            hash *= 16777619u;
        }
    }

    return hash;
}

static void DrawPartySlot(int index, struct Pokemon *mon)
{
    const int slotH = 36;
    const int slotY = 8 + index * (slotH + 4);

    FillRect(8, slotY, W - 16, slotH, COL_PANEL);

    u32 species = GetMonData(mon, MON_DATA_SPECIES);
    if (species == SPECIES_NONE)
        return;

    u32 hp    = GetMonData(mon, MON_DATA_HP);
    u32 maxHp = GetMonData(mon, MON_DATA_MAX_HP);

    // A fainted or freshly hatched mon must not divide by zero.
    if (maxHp == 0)
        return;

    const int barX = 16, barW = W - 48, barH = 10;
    const int barY = slotY + slotH - barH - 8;

    FillRect(barX, barY, barW, barH, COL_HP_BACK);

    u32 filled = (hp * (u32)barW) / maxHp;
    // Any surviving HP should show at least a sliver rather than reading as 0.
    if (filled == 0 && hp > 0)
        filled = 1;

    u16 color = COL_HP_HIGH;
    if (hp * 2 <= maxHp) color = COL_HP_MID;
    if (hp * 5 <= maxHp) color = COL_HP_LOW;

    FillRect(barX, barY, (int)filled, barH, color);

    // Level as a row of ticks: a stand-in until the font blitter lands, but it
    // makes the readout verifiably track the real party.
    u32 level = GetMonData(mon, MON_DATA_LEVEL);
    for (u32 t = 0; t < level / 10 && t < 10; t++)
        FillRect(16 + (int)t * 6, slotY + 6, 4, 6, COL_TEXT);
}

void CtrBottomInit(void)
{
    FillRect(0, 0, W, H, COL_BG);
    sDirty = 1;
    sLastStateHash = 0;
}

void CtrBottomUpdate(const CtrTouchState *touch)
{
    (void)touch;   // Phase 4 wires touch to item use.

    u32 hash = PartyStateHash();
    if (hash == sLastStateHash)
        return;

    sLastStateHash = hash;

    FillRect(0, 0, W, H, COL_BG);
    for (u32 i = 0; i < PARTY_SIZE; i++)
        DrawPartySlot((int)i, &gPlayerParty[i]);

    sDirty = 1;
}

int CtrBottomIsDirty(void)          { return sDirty; }
void CtrBottomClearDirty(void)      { sDirty = 0; }
const u16 *CtrBottomFramebuffer(void) { return sFramebuffer; }
