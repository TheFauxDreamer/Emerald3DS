@ The four event items, 3DS build only (3ds/EVENT_ISLANDS_PLAN.md): the Eon
@ Ticket for Southern Island, the Aurora Ticket for Birth Island, the Old Sea
@ Map for Faraway Island and the Mystic Ticket for Navel Rock. Emerald only ever
@ handed these out through distributions that ended long ago: Record Mixing for
@ the Eon Ticket (ReceiveGiftItem in src/record_mixing.c), Mystery Gift for the
@ other three (gift_*.inc).
@
@ The Lilycove ferry sails to an island only after the Hall of Fame, and only
@ with both the item in the bag and its FLAG_ENABLE_SHIP_* flag
@ (LilycoveCity_Harbor_EventScript_Get*State). The distributions set exactly
@ that, plus FLAG_RECEIVED_* for the three Mystery Gift items, and so does this
@ file.
@
@ Four callers, each behind #if PLATFORM_3DS:
@   Dad, in the post-Hall-of-Fame scene at home (players_house.inc).
@   The Lilycove ferry attendant, for saves already past that scene.
@   The Slateport and Battle Frontier ferry attendants too, because a Battle
@   Frontier player may never talk to the Lilycove one.

@ Each entry point gives whichever items are still needed, and says nothing at
@ all when none are.
Ctr3ds_EventScript_EventTicketsFromDad::
	call Ctr3ds_EventScript_AnyEventTicketNeeded
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	msgbox Ctr3ds_Text_DadTheseCameForYouToo, MSGBOX_DEFAULT
	call Ctr3ds_EventScript_GiveEventItems
	msgbox Ctr3ds_Text_DadForUseAtLilycove, MSGBOX_DEFAULT
	return

@ Lilycove's own first-time ticket dialogue follows this one.
Ctr3ds_EventScript_EventTicketsAtLilycove::
	call Ctr3ds_EventScript_AnyEventTicketNeeded
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	call Ctr3ds_EventScript_EventTicketsAtPort
	return

@ Slateport and the Battle Frontier, which don't sail to the islands.
Ctr3ds_EventScript_EventTicketsAtOtherPort::
	call Ctr3ds_EventScript_AnyEventTicketNeeded
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	call Ctr3ds_EventScript_EventTicketsAtPort
	msgbox Ctr3ds_Text_ForUseAtLilycove, MSGBOX_DEFAULT
	return

Ctr3ds_EventScript_EventTicketsAtPort::
	msgbox Ctr3ds_Text_LeftWithUsForYou, MSGBOX_DEFAULT
	call Ctr3ds_EventScript_GiveEventItems
	return

@ VAR_RESULT: TRUE if any of the four still needs handing over. Never before
@ the Hall of Fame, whatever the caller.
Ctr3ds_EventScript_AnyEventTicketNeeded::
	setvar VAR_RESULT, FALSE
	goto_if_unset FLAG_SYS_GAME_CLEAR, Common_EventScript_NopReturn
	call Ctr3ds_EventScript_EonTicketNeeded
	goto_if_eq VAR_RESULT, TRUE, Common_EventScript_NopReturn
	call Ctr3ds_EventScript_AuroraTicketNeeded
	goto_if_eq VAR_RESULT, TRUE, Common_EventScript_NopReturn
	call Ctr3ds_EventScript_OldSeaMapNeeded
	goto_if_eq VAR_RESULT, TRUE, Common_EventScript_NopReturn
	call Ctr3ds_EventScript_MysticTicketNeeded
	return

@ VAR_RESULT: TRUE if the item is needed. It is, unless the player already has
@ both it and its enable flag. What the player has caught is not consulted,
@ unlike the gift scripts: a catch on an island leaves its item in the bag,
@ since nothing removes key items, so a catch check could only ever strand a
@ save whose flags and bag disagree, and New Adventures Await counts the items.
Ctr3ds_EventScript_EonTicketNeeded::
	goto_if_unset FLAG_ENABLE_SHIP_SOUTHERN_ISLAND, Ctr3ds_EventScript_EventItemIsNeeded
	checkitem ITEM_EON_TICKET
	goto Ctr3ds_EventScript_NeededIfNotInBag
	end

Ctr3ds_EventScript_AuroraTicketNeeded::
	goto_if_unset FLAG_ENABLE_SHIP_BIRTH_ISLAND, Ctr3ds_EventScript_EventItemIsNeeded
	checkitem ITEM_AURORA_TICKET
	goto Ctr3ds_EventScript_NeededIfNotInBag
	end

Ctr3ds_EventScript_OldSeaMapNeeded::
	goto_if_unset FLAG_ENABLE_SHIP_FARAWAY_ISLAND, Ctr3ds_EventScript_EventItemIsNeeded
	checkitem ITEM_OLD_SEA_MAP
	goto Ctr3ds_EventScript_NeededIfNotInBag
	end

Ctr3ds_EventScript_MysticTicketNeeded::
	goto_if_unset FLAG_ENABLE_SHIP_NAVEL_ROCK, Ctr3ds_EventScript_EventItemIsNeeded
	checkitem ITEM_MYSTIC_TICKET
	goto Ctr3ds_EventScript_NeededIfNotInBag
	end

@ After a checkitem.
Ctr3ds_EventScript_NeededIfNotInBag::
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemIsNeeded
	setvar VAR_RESULT, FALSE
	return

Ctr3ds_EventScript_EventItemIsNeeded::
	setvar VAR_RESULT, TRUE
	return

@ Stops at a full bag, which then says so once rather than once for each item
@ left.
Ctr3ds_EventScript_GiveEventItems::
	call Ctr3ds_EventScript_TryGiveEonTicket
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemsBagFull
	call Ctr3ds_EventScript_TryGiveAuroraTicket
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemsBagFull
	call Ctr3ds_EventScript_TryGiveOldSeaMap
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemsBagFull
	call Ctr3ds_EventScript_TryGiveMysticTicket
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemsBagFull
	return

@ Not Common_EventScript_ShowBagIsFull, which ends the script: every caller
@ carries on afterwards.
Ctr3ds_EventScript_EventItemsBagFull::
	msgbox gText_TooBadBagIsFull, MSGBOX_DEFAULT
	return

@ One item each. VAR_RESULT comes back FALSE only if the bag was full, and then
@ no flags are set, so a ferry attendant can hand the item over later. An item
@ already in the bag only gets its flags.
Ctr3ds_EventScript_TryGiveEonTicket::
	call Ctr3ds_EventScript_EonTicketNeeded
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemNotNeeded
	setvar VAR_0x8000, ITEM_EON_TICKET
	call Ctr3ds_EventScript_GiveEventItemIfMissing
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	setflag FLAG_ENABLE_SHIP_SOUTHERN_ISLAND
	return

Ctr3ds_EventScript_TryGiveAuroraTicket::
	call Ctr3ds_EventScript_AuroraTicketNeeded
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemNotNeeded
	setvar VAR_0x8000, ITEM_AURORA_TICKET
	call Ctr3ds_EventScript_GiveEventItemIfMissing
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	setflag FLAG_ENABLE_SHIP_BIRTH_ISLAND
	setflag FLAG_RECEIVED_AURORA_TICKET
	return

Ctr3ds_EventScript_TryGiveOldSeaMap::
	call Ctr3ds_EventScript_OldSeaMapNeeded
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemNotNeeded
	setvar VAR_0x8000, ITEM_OLD_SEA_MAP
	call Ctr3ds_EventScript_GiveEventItemIfMissing
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	setflag FLAG_ENABLE_SHIP_FARAWAY_ISLAND
	setflag FLAG_RECEIVED_OLD_SEA_MAP
	return

Ctr3ds_EventScript_TryGiveMysticTicket::
	call Ctr3ds_EventScript_MysticTicketNeeded
	goto_if_eq VAR_RESULT, FALSE, Ctr3ds_EventScript_EventItemNotNeeded
	setvar VAR_0x8000, ITEM_MYSTIC_TICKET
	call Ctr3ds_EventScript_GiveEventItemIfMissing
	goto_if_eq VAR_RESULT, FALSE, Common_EventScript_NopReturn
	setflag FLAG_ENABLE_SHIP_NAVEL_ROCK
	setflag FLAG_RECEIVED_MYSTIC_TICKET
	return

@ Nothing to give, so the bag was not full.
Ctr3ds_EventScript_EventItemNotNeeded::
	setvar VAR_RESULT, TRUE
	return

@ VAR_0x8000: the item, which giveitem overwrites with itself, so callers see
@ nothing a plain giveitem would not do. VAR_RESULT: TRUE if it is in the bag
@ now. giveitem plays the obtain message and fanfare, and shows nothing when
@ the bag is full.
Ctr3ds_EventScript_GiveEventItemIfMissing::
	checkitem VAR_0x8000
	goto_if_eq VAR_RESULT, TRUE, Common_EventScript_NopReturn
	giveitem VAR_0x8000
	return

Ctr3ds_Text_DadTheseCameForYouToo:
	.string "DAD: Oh, and these came for you, too.\p"
	.string "More TICKETS, and an old map…\n"
	.string "There was no sender's name on them.$"

Ctr3ds_Text_DadForUseAtLilycove:
	.string "DAD: They seem to be for use at the\n"
	.string "port in LILYCOVE.$"

Ctr3ds_Text_LeftWithUsForYou:
	.string "Oh, {PLAYER}, isn't it?\p"
	.string "Some things were left with us for\n"
	.string "you. Here you are.$"

Ctr3ds_Text_ForUseAtLilycove:
	.string "They appear to be for use at the\n"
	.string "LILYCOVE CITY port.$"
