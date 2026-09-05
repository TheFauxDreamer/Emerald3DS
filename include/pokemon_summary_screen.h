#ifndef GUARD_POKEMON_SUMMARY_SCREEN_H
#define GUARD_POKEMON_SUMMARY_SCREEN_H

#include "main.h"

extern u8 gLastViewedMonIndex;

extern const u8 *const gMoveDescriptionPointers[];

#if PLATFORM_3DS
// The move type icon sheet, for the second screen. One 32x16 4bpp icon per type
// at 0x100 bytes, in type order; the palette is three 16-colour banks and
// Ctr3dsGetTypeIconPalBank says which one a type uses. See the PLATFORM_3DS
// block in src/pokemon_summary_screen.c.
#define CTR_TYPE_ICON_W     32
#define CTR_TYPE_ICON_H     16
#define CTR_TYPE_ICON_BYTES 0x100

void Ctr3dsGetTypeIconGfx(const u32 **gfxLZ, const u32 **palLZ);
u8   Ctr3dsGetTypeIconPalBank(u8 typeId);
#endif

extern const u8 *const gNatureNamePointers[];

void ShowPokemonSummaryScreen(u8 mode, void *mons, u8 monIndex, u8 maxMonIndex, void (*callback)(void));
void ShowSelectMovePokemonSummaryScreen(struct Pokemon *mons, u8 monIndex, u8 maxMonIndex, void (*callback)(void), u16 newMove);
void ShowPokemonSummaryScreenHandleDeoxys(u8 mode, struct BoxPokemon *mons, u8 monIndex, u8 maxMonIndex, void (*callback)(void));
u8 GetMoveSlotToReplace(void);
void SummaryScreen_SetAnimDelayTaskId(u8 taskId);

// The Pokémon Summary Screen can operate in different modes. Certain features,
// such as move re-ordering, are available in the different modes.
enum PokemonSummaryScreenMode
{
    SUMMARY_MODE_NORMAL,
    SUMMARY_MODE_LOCK_MOVES,
    SUMMARY_MODE_BOX,
    SUMMARY_MODE_SELECT_MOVE,
};

#endif // GUARD_POKEMON_SUMMARY_SCREEN_H
