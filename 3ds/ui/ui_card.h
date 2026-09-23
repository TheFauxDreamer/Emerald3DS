// The trainer card, drawn on the touch screen from the data a link already
// exchanged.
//
// Every cable-club link-up ends by copying each peer's card into gTrainerCards
// (Task_LinkupAwaitTrainerCardData, src/cable_club.c), including the local
// slot, so both cards are there before a trade or a battle starts. Nothing here
// asks for anything over the wire.
//
// The card is the GBA's own art: the same tiles, tilemaps and star-tier
// palettes that src/trainer_card.c uses, so it looks like the card the game
// shows. It is 240x160, a whole GBA screen, and it is drawn at 1:1 because
// faithful tile art needs an integer scale.

#ifndef CTR_UI_CARD_H
#define CTR_UI_CARD_H

#include "global.h"
#include "../bridge.h"

// A GBA screen.
#define UI_CARD_W 240
#define UI_CARD_H 160

// The thumbnail, at 1:4. No text is drawn at that size; it is the card's shape
// and colour, which is what tells one card from another at a glance.
#define UI_CARD_THUMB_W (UI_CARD_W / 4)
#define UI_CARD_THUMB_H (UI_CARD_H / 4)

// Is gTrainerCards[cardId] this link's card, rather than the last one's?
//
// gTrainerCards is never cleared, and both link flags go true before the card
// block arrives, so "a link is up" does not mean "this card is theirs". See the
// definition for the test.
int UiCardAvailable(int cardId);

// One card at 1:1. `back` picks the profile side.
void UiCardDraw(int x, int y, int cardId, int back);

// The same card at 1:4, front only.
void UiCardThumb(int x, int y, int cardId);

#endif // CTR_UI_CARD_H
