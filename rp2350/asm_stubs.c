// Stubs for symbols that live in GBA assembly files we do not build on RP2350:
//   m4a_1.s        - the m4a sound engine (audio is stubbed, like WASM)
//   crt0.s         - IntrMain (GBA interrupt dispatcher; we drive frames directly)
//   rom_header.s   - cartridge header fields
//   libgcnmultiboot.s + multiboot data - GameCube/e-reader/Colosseum link blobs
//
// The m4a/multiboot/cry paths are unreachable in normal play (they need a link
// cable, e-reader, or mystery-gift transfer) and are never hit during Phase-1
// bring-up, so no-ops / minimal data are sufficient to satisfy the linker.
#include "global.h"
#include "main.h"
#include "gba/m4a_internal.h"
#include "libgcnmultiboot.h"
#include "constants/global.h"

// NOTE: The m4a sound engine (the m4a_1.s assembly core: ply_*, MPlayMain,
// SoundMain, SoundMainRAM, TrackStop, ChnVolSetAsm, RealClearChain, ply_note,
// umul3232H32, ...) is now provided in C by rp2350/m4a_1.c, and the high-level
// engine (TrkVolPitSet, FadeOutBody, SampleFreqSet, the cry control, the ply_x*
// commands) by the real src/m4a.c path. They are no longer stubbed here.

// --- GBA interrupt dispatcher (crt0.s) --------------------------------------
// Never executed: RP2350 drives frames via the WasmRunFrame superloop. Present
// only so InitIntrHandlers() can store it in INTR_VECTOR.
void IntrMain(void) {}

// --- Cartridge header fields (rom_header.s) ---------------------------------
const u8 RomHeaderGameCode[GAME_CODE_LENGTH] = _("BPEE");
const u8 RomHeaderSoftwareVersion = 0;

// --- GameCube / e-reader / Colosseum multiboot (libgcnmultiboot.s + blobs) ---
void GameCubeMultiBoot_Main(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_ExecuteProgram(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_Init(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_HandleSerialInterrupt(struct GcmbStruct *p) { (void)p; }
void GameCubeMultiBoot_Quit(void) {}

// Multiboot program blobs: real sizes are declared in their headers (used by
// sizeof on code paths we never reach). Minimal definitions satisfy the linker.
const u16 gMultiBootProgram_PokemonColosseum_Start[1] = {0};
const u8 gMultiBootProgram_EReader_Start[1] = {0};
const u8 gMultiBootProgram_EReader_End[1] = {0};
const u8 gMultiBootProgram_BerryGlitchFix_Start[1] = {0};
const u8 gMultiBootProgram_BerryGlitchFix_End[1] = {0};

// --- Data tables the WASM build leaves as zero-stub (--allow-undefined) -------
// The contest-AI / mystery-event paths aren't reached during bring-up, so empty
// definitions suffice (matches WASM behaviour). Types need only match by symbol.
// NOTE: voicegroup_dummy / gCryTable / gCryTable_Reverse are now provided for
// real by data/sound_data.s (assembled into the archive), so they are no longer
// stubbed here.
const u8 *gContestAI_ScriptsTable[1] = {0};
void *gMysteryEventScriptCmdTable[1] = {0};
void *gMysteryEventScriptCmdTableEnd[1] = {0};
