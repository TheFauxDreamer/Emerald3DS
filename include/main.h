#ifndef GUARD_MAIN_H
#define GUARD_MAIN_H

typedef void (*MainCallback)(void);
typedef void (*IntrCallback)(void);
typedef void (*IntrFunc)(void);

struct Main
{
    /*0x000*/ MainCallback callback1;
    /*0x004*/ MainCallback callback2;

    /*0x008*/ MainCallback savedCallback;

    /*0x00C*/ IntrCallback vblankCallback;
    /*0x010*/ IntrCallback hblankCallback;
    /*0x014*/ IntrCallback vcountCallback;
    /*0x018*/ IntrCallback serialCallback;

    /*0x01C*/ vu16 intrCheck;

    /*0x020*/ u32 vblankCounter1;
    /*0x024*/ u32 vblankCounter2;

    /*0x028*/ u16 heldKeysRaw;           // held keys without L=A remapping
    /*0x02A*/ u16 newKeysRaw;            // newly pressed keys without L=A remapping
    /*0x02C*/ u16 heldKeys;              // held keys with L=A remapping
    /*0x02E*/ u16 newKeys;               // newly pressed keys with L=A remapping
    /*0x030*/ u16 newAndRepeatedKeys;    // newly pressed keys plus key repeat
    /*0x032*/ u16 keyRepeatCounter;      // counts down to 0, triggering key repeat
    /*0x034*/ bool16 watchedKeysPressed; // whether one of the watched keys was pressed
    /*0x036*/ u16 watchedKeysMask;       // bit mask for watched keys

    /*0x038*/ struct OamData oamBuffer[128];

    /*0x438*/ u8 state;

    /*0x439*/ u8 oamLoadDisabled:1;
    /*0x439*/ u8 inBattle:1;
    /*0x439*/ u8 anyLinkBattlerHasFrontierPass:1;
};

#define GAME_CODE_LENGTH 4
extern const u8 gGameVersion;
extern const u8 gGameLanguage;
extern const u8 RomHeaderGameCode[GAME_CODE_LENGTH];
extern const u8 RomHeaderSoftwareVersion;

extern u16 gKeyRepeatStartDelay;
extern bool8 gLinkTransferringData;
extern struct Main gMain;
extern u16 gKeyRepeatContinueDelay;
extern bool8 gSoftResetDisabled;
extern IntrFunc gIntrTable[];
extern u8 gLinkVSyncDisabled;
extern u32 IntrMain_Buffer[];
extern s8 gPcmDmaCounter;

void AgbMain(void);
void SetMainCallback2(MainCallback callback);
void InitKeys(void);
void SetVBlankCallback(IntrCallback callback);
void SetHBlankCallback(IntrCallback callback);
void SetVCountCallback(IntrCallback callback);
void SetSerialCallback(IntrCallback callback);

// A scene's VBlank callback outlives the scene's own state by at least one call.
// The exit path of every heap-backed scene does the same three things together
// -- SetMainCallback2(returnCallback), DestroyTask(), FREE_AND_SET_NULL(state)
// -- and the callback stays installed until the NEXT scene's first setup state
// runs. VBlankIntr() therefore calls it again, this frame, with the pointer it
// reads already NULL.
//
// On a GBA that is invisible: address 0 is BIOS space, a read there returns a
// stale opcode rather than faulting, so the callback writes a junk scroll
// offset for one frame that nothing ever sees. In wasm address 0 is inside the
// linear memory, so likewise. On the 3DS's ARM11 nothing is mapped below the
// code segment and the identical read is an instant data abort -- which is how
// VBlankCB_NamingScreen faulted reading 0x1E18, exactly
// NULL + offsetof(struct NamingScreenData, bg1vOffset), on confirming a name.
//
// The guard goes in the callback rather than at the free sites because the
// callback is the only code that knows which pointer it needs, and because a
// stale callback is reached from more than just the frame that freed it.
// Skipping the frame is what the GBA effectively does anyway: the scene is
// gone, so there are no scroll offsets or window bounds left to write.
#if WASM || RP2350
#define VBLANK_REQUIRE(ptr) do { if ((ptr) == NULL) return; } while (0)
#else
#define VBLANK_REQUIRE(ptr) ((void)0)
#endif

void InitFlashTimer(void);
void SetTrainerHillVBlankCounter(u32 *counter);
void ClearTrainerHillVBlankCounter(void);
void DoSoftReset(void);
void ClearPokemonCrySongs(void);
void RestoreSerialTimer3IntrHandlers(void);
void StartTimer1(void);
void SeedRngAndSetTrainerId(void);
u16 GetGeneratedTrainerIdLower(void);

#endif // GUARD_MAIN_H
