#include "global.h"

#if WASM || RP2350

#include "sprite.h"
#include "constants/field_effects.h"

typedef u32 (*FieldEffectFunc)(void);

struct FieldEffectScriptCallNative
{
    u8 cmd;
    FieldEffectFunc func;
    u8 end;
} __attribute__((packed));

struct FieldEffectScriptLoadFadedPalCallNative
{
    u8 cmd;
    const struct SpritePalette *palette;
    FieldEffectFunc func;
    u8 end;
} __attribute__((packed));

struct FieldEffectScriptLoadFadedPalThenCallNative
{
    u8 loadFadedPalCmd;
    const struct SpritePalette *firstPalette;
    u8 loadFadedPalCallNativeCmd;
    const struct SpritePalette *secondPalette;
    FieldEffectFunc func;
    u8 end;
} __attribute__((packed));

struct FieldEffectScriptLoadPalThenCallNative
{
    u8 loadPalCmd;
    const struct SpritePalette *palette;
    u8 callNativeCmd;
    FieldEffectFunc func;
    u8 end;
} __attribute__((packed));

#define FIELD_EFFECT_SCRIPT_END 4
#define FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL 1
#define FIELD_EFFECT_SCRIPT_LOAD_PAL 2
#define FIELD_EFFECT_SCRIPT_CALL_NATIVE 3
#define FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL_CALL_NATIVE 7

#define SCRIPT_CALL_NATIVE(name, func) \
    static const struct FieldEffectScriptCallNative sScript_##name = { \
        FIELD_EFFECT_SCRIPT_CALL_NATIVE, func, FIELD_EFFECT_SCRIPT_END \
    }

#define SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(name, palette, func) \
    static const struct FieldEffectScriptLoadFadedPalCallNative sScript_##name = { \
        FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL_CALL_NATIVE, palette, func, FIELD_EFFECT_SCRIPT_END \
    }

extern const struct SpritePalette gSpritePalette_Ash;
extern const struct SpritePalette gSpritePalette_CutGrass;
extern const struct SpritePalette gSpritePalette_GeneralFieldEffect0;
extern const struct SpritePalette gSpritePalette_GeneralFieldEffect1;
extern const struct SpritePalette gSpritePalette_HofMonitor;
extern const struct SpritePalette gSpritePalette_Pokeball;
extern const struct SpritePalette gSpritePalette_PokeballGlow;
extern const struct SpritePalette gSpritePalette_SandPillar;
extern const struct SpritePalette gSpritePalette_SecretPower_Cave;
extern const struct SpritePalette gSpritePalette_SecretPower_Plant;
extern const struct SpritePalette gSpritePalette_SmallSparkle;

extern u32 FldEff_Ash(void);
extern u32 FldEff_AshLaunch(void);
extern u32 FldEff_AshPuff(void);
extern u32 FldEff_BerryTreeGrowthSparkle(void);
extern u32 FldEff_BikeTireTracks(void);
extern u32 FldEff_Bubbles(void);
extern u32 FldEff_CutGrass(void);
extern u32 FldEff_DeepSandFootprints(void);
extern u32 FldEff_DestroyDeoxysRock(void);
extern u32 FldEff_Dust(void);
extern u32 FldEff_ExclamationMarkIcon(void);
extern u32 FldEff_FeetInFlowingWater(void);
extern u32 FldEff_FieldMoveShowMon(void);
extern u32 FldEff_FieldMoveShowMonInit(void);
extern u32 FldEff_FlyIn(void);
extern u32 FldEff_HallOfFameRecord(void);
extern u32 FldEff_HeartIcon(void);
extern u32 FldEff_HotSpringsWater(void);
extern u32 FldEff_JumpBigSplash(void);
extern u32 FldEff_JumpLongGrass(void);
extern u32 FldEff_JumpSmallSplash(void);
extern u32 FldEff_JumpTallGrass(void);
extern u32 FldEff_LongGrass(void);
extern u32 FldEff_MoveDeoxysRock(void);
extern u32 FldEff_NPCFlyOut(void);
extern u32 FldEff_Nop47(void);
extern u32 FldEff_Nop48(void);
extern u32 FldEff_PokeballTrail(void);
extern u32 FldEff_PokecenterHeal(void);
extern u32 FldEff_QuestionMarkIcon(void);
extern u32 FldEff_RayquazaSpotlight(void);
extern u32 FldEff_Ripple(void);
extern u32 FldEff_SandFootprints(void);
extern u32 FldEff_SandPile(void);
extern u32 FldEff_SandPillar(void);
extern u32 FldEff_SecretBasePCTurnOn(void);
extern u32 FldEff_SecretPowerCave(void);
extern u32 FldEff_SecretPowerShrub(void);
extern u32 FldEff_SecretPowerTree(void);
extern u32 FldEff_Shadow(void);
extern u32 FldEff_ShortGrass(void);
extern u32 FldEff_Sparkle(void);
extern u32 FldEff_Splash(void);
extern u32 FldEff_SurfBlob(void);
extern u32 FldEff_SweetScent(void);
extern u32 FldEff_TallGrass(void);
extern u32 FldEff_UnusedGrass(void);
extern u32 FldEff_UnusedGrass2(void);
extern u32 FldEff_UnusedSand(void);
extern u32 FldEff_UseCutOnGrass(void);
extern u32 FldEff_UseCutOnTree(void);
extern u32 FldEff_UseDig(void);
extern u32 FldEff_UseDive(void);
extern u32 FldEff_UseFly(void);
extern u32 FldEff_UsePuzzleEffect(void);
extern u32 FldEff_UseRockSmash(void);
extern u32 FldEff_UseSecretPowerCave(void);
extern u32 FldEff_UseSecretPowerShrub(void);
extern u32 FldEff_UseSecretPowerTree(void);
extern u32 FldEff_UseStrength(void);
extern u32 FldEff_UseSurf(void);
extern u32 FldEff_UseTeleport(void);
extern u32 FldEff_UseWaterfall(void);
extern u32 FldEff_WaterSurfacing(void);
extern u32 ShowMountainDisguiseFieldEffect(void);
extern u32 ShowSandDisguiseFieldEffect(void);
extern u32 ShowTreeDisguiseFieldEffect(void);

SCRIPT_CALL_NATIVE(ExclamationMarkIcon1, FldEff_ExclamationMarkIcon);
SCRIPT_CALL_NATIVE(UseCutOnTallGrass, FldEff_UseCutOnGrass);
SCRIPT_CALL_NATIVE(UseCutOnTree, FldEff_UseCutOnTree);
SCRIPT_CALL_NATIVE(Shadow, FldEff_Shadow);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(TallGrass, &gSpritePalette_GeneralFieldEffect1, FldEff_TallGrass);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(Ripple, &gSpritePalette_GeneralFieldEffect1, FldEff_Ripple);
SCRIPT_CALL_NATIVE(FieldMoveShowMon, FldEff_FieldMoveShowMon);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(Ash, &gSpritePalette_GeneralFieldEffect1, FldEff_Ash);
SCRIPT_CALL_NATIVE(SurfBlob, FldEff_SurfBlob);
SCRIPT_CALL_NATIVE(UseSurf, FldEff_UseSurf);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(GroundImpactDust, &gSpritePalette_GeneralFieldEffect0, FldEff_Dust);
SCRIPT_CALL_NATIVE(UseSecretPowerCave, FldEff_UseSecretPowerCave);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(JumpTallGrass, &gSpritePalette_GeneralFieldEffect1, FldEff_JumpTallGrass);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(SandFootprints, &gSpritePalette_GeneralFieldEffect0, FldEff_SandFootprints);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(JumpBigSplash, &gSpritePalette_GeneralFieldEffect0, FldEff_JumpBigSplash);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(Splash, &gSpritePalette_GeneralFieldEffect0, FldEff_Splash);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(JumpSmallSplash, &gSpritePalette_GeneralFieldEffect0, FldEff_JumpSmallSplash);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(LongGrass, &gSpritePalette_GeneralFieldEffect1, FldEff_LongGrass);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(JumpLongGrass, &gSpritePalette_GeneralFieldEffect1, FldEff_JumpLongGrass);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(UnusedGrass, &gSpritePalette_GeneralFieldEffect1, FldEff_UnusedGrass);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(UnusedGrass2, &gSpritePalette_GeneralFieldEffect1, FldEff_UnusedGrass2);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(UnusedSand, &gSpritePalette_GeneralFieldEffect0, FldEff_UnusedSand);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(WaterSurfacing, &gSpritePalette_GeneralFieldEffect0, FldEff_WaterSurfacing);
SCRIPT_CALL_NATIVE(BerryTreeGrowthSparkle, FldEff_BerryTreeGrowthSparkle);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(DeepSandFootprints, &gSpritePalette_GeneralFieldEffect0, FldEff_DeepSandFootprints);
static const struct FieldEffectScriptLoadFadedPalThenCallNative sScript_PokeCenterHeal = {
    FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL, &gSpritePalette_PokeballGlow,
    FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL_CALL_NATIVE, &gSpritePalette_GeneralFieldEffect0,
    FldEff_PokecenterHeal, FIELD_EFFECT_SCRIPT_END
};
SCRIPT_CALL_NATIVE(UseSecretPowerTree, FldEff_UseSecretPowerTree);
SCRIPT_CALL_NATIVE(UseSecretPowerShrub, FldEff_UseSecretPowerShrub);
SCRIPT_CALL_NATIVE(TreeDisguise, ShowTreeDisguiseFieldEffect);
SCRIPT_CALL_NATIVE(MountainDisguise, ShowMountainDisguiseFieldEffect);
SCRIPT_CALL_NATIVE(NPCUseFly, FldEff_NPCFlyOut);
SCRIPT_CALL_NATIVE(UseFly, FldEff_UseFly);
SCRIPT_CALL_NATIVE(FlyIn, FldEff_FlyIn);
SCRIPT_CALL_NATIVE(QuestionMarkIcon, FldEff_QuestionMarkIcon);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(FeetInFlowingWater, &gSpritePalette_GeneralFieldEffect0, FldEff_FeetInFlowingWater);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(BikeTireTracks, &gSpritePalette_GeneralFieldEffect0, FldEff_BikeTireTracks);
SCRIPT_CALL_NATIVE(SandDisguisePlaceholder, ShowSandDisguiseFieldEffect);
SCRIPT_CALL_NATIVE(UseRockSmash, FldEff_UseRockSmash);
SCRIPT_CALL_NATIVE(UseDig, FldEff_UseDig);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(SandPile, &gSpritePalette_GeneralFieldEffect0, FldEff_SandPile);
SCRIPT_CALL_NATIVE(UseStrength, FldEff_UseStrength);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(ShortGrass, &gSpritePalette_GeneralFieldEffect1, FldEff_ShortGrass);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(HotSpringsWater, &gSpritePalette_GeneralFieldEffect1, FldEff_HotSpringsWater);
SCRIPT_CALL_NATIVE(UseWaterfall, FldEff_UseWaterfall);
SCRIPT_CALL_NATIVE(UseDive, FldEff_UseDive);
static const struct FieldEffectScriptLoadPalThenCallNative sScript_PokeballTrail = {
    FIELD_EFFECT_SCRIPT_LOAD_PAL, &gSpritePalette_Pokeball,
    FIELD_EFFECT_SCRIPT_CALL_NATIVE, FldEff_PokeballTrail, FIELD_EFFECT_SCRIPT_END
};
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(HeartIcon, &gSpritePalette_GeneralFieldEffect0, FldEff_HeartIcon);
SCRIPT_CALL_NATIVE(Nop47, FldEff_Nop47);
SCRIPT_CALL_NATIVE(Nop48, FldEff_Nop48);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(AshPuff, &gSpritePalette_Ash, FldEff_AshPuff);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(AshLaunch, &gSpritePalette_Ash, FldEff_AshLaunch);
SCRIPT_CALL_NATIVE(SweetScent, FldEff_SweetScent);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(SandPillar, &gSpritePalette_SandPillar, FldEff_SandPillar);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(Bubbles, &gSpritePalette_GeneralFieldEffect0, FldEff_Bubbles);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(Sparkle, &gSpritePalette_SmallSparkle, FldEff_Sparkle);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(ShowSecretPowerCave, &gSpritePalette_SecretPower_Cave, FldEff_SecretPowerCave);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(ShowSecretPowerTree, &gSpritePalette_SecretPower_Plant, FldEff_SecretPowerTree);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(ShowSecretPowerShrub, &gSpritePalette_SecretPower_Plant, FldEff_SecretPowerShrub);
SCRIPT_LOAD_FADED_PAL_CALL_NATIVE(ShowCutGrass, &gSpritePalette_CutGrass, FldEff_CutGrass);
SCRIPT_CALL_NATIVE(FieldMoveShowMonInit, FldEff_FieldMoveShowMonInit);
SCRIPT_CALL_NATIVE(UsePuzzleEffect, FldEff_UsePuzzleEffect);
SCRIPT_CALL_NATIVE(SecretBaseBootPC, FldEff_SecretBasePCTurnOn);
static const struct FieldEffectScriptLoadFadedPalThenCallNative sScript_HallOfFameRecord = {
    FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL, &gSpritePalette_PokeballGlow,
    FIELD_EFFECT_SCRIPT_LOAD_FADED_PAL_CALL_NATIVE, &gSpritePalette_HofMonitor,
    FldEff_HallOfFameRecord, FIELD_EFFECT_SCRIPT_END
};
SCRIPT_CALL_NATIVE(UseTeleport, FldEff_UseTeleport);
SCRIPT_CALL_NATIVE(RayquazaSpotlight, FldEff_RayquazaSpotlight);
SCRIPT_CALL_NATIVE(DestroyDeoxysRock, FldEff_DestroyDeoxysRock);
SCRIPT_CALL_NATIVE(MoveDeoxysRock, FldEff_MoveDeoxysRock);

#define SCRIPT_PTR(name) ((u8 *)&sScript_##name)

u8 *gFieldEffectScriptPointers[] = {
    [FLDEFF_EXCLAMATION_MARK_ICON] = SCRIPT_PTR(ExclamationMarkIcon1),
    [FLDEFF_USE_CUT_ON_GRASS] = SCRIPT_PTR(UseCutOnTallGrass),
    [FLDEFF_USE_CUT_ON_TREE] = SCRIPT_PTR(UseCutOnTree),
    [FLDEFF_SHADOW] = SCRIPT_PTR(Shadow),
    [FLDEFF_TALL_GRASS] = SCRIPT_PTR(TallGrass),
    [FLDEFF_RIPPLE] = SCRIPT_PTR(Ripple),
    [FLDEFF_FIELD_MOVE_SHOW_MON] = SCRIPT_PTR(FieldMoveShowMon),
    [FLDEFF_ASH] = SCRIPT_PTR(Ash),
    [FLDEFF_SURF_BLOB] = SCRIPT_PTR(SurfBlob),
    [FLDEFF_USE_SURF] = SCRIPT_PTR(UseSurf),
    [FLDEFF_DUST] = SCRIPT_PTR(GroundImpactDust),
    [FLDEFF_USE_SECRET_POWER_CAVE] = SCRIPT_PTR(UseSecretPowerCave),
    [FLDEFF_JUMP_TALL_GRASS] = SCRIPT_PTR(JumpTallGrass),
    [FLDEFF_SAND_FOOTPRINTS] = SCRIPT_PTR(SandFootprints),
    [FLDEFF_JUMP_BIG_SPLASH] = SCRIPT_PTR(JumpBigSplash),
    [FLDEFF_SPLASH] = SCRIPT_PTR(Splash),
    [FLDEFF_JUMP_SMALL_SPLASH] = SCRIPT_PTR(JumpSmallSplash),
    [FLDEFF_LONG_GRASS] = SCRIPT_PTR(LongGrass),
    [FLDEFF_JUMP_LONG_GRASS] = SCRIPT_PTR(JumpLongGrass),
    [FLDEFF_UNUSED_GRASS] = SCRIPT_PTR(UnusedGrass),
    [FLDEFF_UNUSED_GRASS_2] = SCRIPT_PTR(UnusedGrass2),
    [FLDEFF_UNUSED_SAND] = SCRIPT_PTR(UnusedSand),
    [FLDEFF_WATER_SURFACING] = SCRIPT_PTR(WaterSurfacing),
    [FLDEFF_BERRY_TREE_GROWTH_SPARKLE] = SCRIPT_PTR(BerryTreeGrowthSparkle),
    [FLDEFF_DEEP_SAND_FOOTPRINTS] = SCRIPT_PTR(DeepSandFootprints),
    [FLDEFF_POKECENTER_HEAL] = SCRIPT_PTR(PokeCenterHeal),
    [FLDEFF_USE_SECRET_POWER_TREE] = SCRIPT_PTR(UseSecretPowerTree),
    [FLDEFF_USE_SECRET_POWER_SHRUB] = SCRIPT_PTR(UseSecretPowerShrub),
    [FLDEFF_TREE_DISGUISE] = SCRIPT_PTR(TreeDisguise),
    [FLDEFF_MOUNTAIN_DISGUISE] = SCRIPT_PTR(MountainDisguise),
    [FLDEFF_NPCFLY_OUT] = SCRIPT_PTR(NPCUseFly),
    [FLDEFF_USE_FLY] = SCRIPT_PTR(UseFly),
    [FLDEFF_FLY_IN] = SCRIPT_PTR(FlyIn),
    [FLDEFF_QUESTION_MARK_ICON] = SCRIPT_PTR(QuestionMarkIcon),
    [FLDEFF_FEET_IN_FLOWING_WATER] = SCRIPT_PTR(FeetInFlowingWater),
    [FLDEFF_BIKE_TIRE_TRACKS] = SCRIPT_PTR(BikeTireTracks),
    [FLDEFF_SAND_DISGUISE] = SCRIPT_PTR(SandDisguisePlaceholder),
    [FLDEFF_USE_ROCK_SMASH] = SCRIPT_PTR(UseRockSmash),
    [FLDEFF_USE_DIG] = SCRIPT_PTR(UseDig),
    [FLDEFF_SAND_PILE] = SCRIPT_PTR(SandPile),
    [FLDEFF_USE_STRENGTH] = SCRIPT_PTR(UseStrength),
    [FLDEFF_SHORT_GRASS] = SCRIPT_PTR(ShortGrass),
    [FLDEFF_HOT_SPRINGS_WATER] = SCRIPT_PTR(HotSpringsWater),
    [FLDEFF_USE_WATERFALL] = SCRIPT_PTR(UseWaterfall),
    [FLDEFF_USE_DIVE] = SCRIPT_PTR(UseDive),
    [FLDEFF_POKEBALL_TRAIL] = SCRIPT_PTR(PokeballTrail),
    [FLDEFF_HEART_ICON] = SCRIPT_PTR(HeartIcon),
    [FLDEFF_NOP_47] = SCRIPT_PTR(Nop47),
    [FLDEFF_NOP_48] = SCRIPT_PTR(Nop48),
    [FLDEFF_ASH_PUFF] = SCRIPT_PTR(AshPuff),
    [FLDEFF_ASH_LAUNCH] = SCRIPT_PTR(AshLaunch),
    [FLDEFF_SWEET_SCENT] = SCRIPT_PTR(SweetScent),
    [FLDEFF_SAND_PILLAR] = SCRIPT_PTR(SandPillar),
    [FLDEFF_BUBBLES] = SCRIPT_PTR(Bubbles),
    [FLDEFF_SPARKLE] = SCRIPT_PTR(Sparkle),
    [FLDEFF_SECRET_POWER_CAVE] = SCRIPT_PTR(ShowSecretPowerCave),
    [FLDEFF_SECRET_POWER_TREE] = SCRIPT_PTR(ShowSecretPowerTree),
    [FLDEFF_SECRET_POWER_SHRUB] = SCRIPT_PTR(ShowSecretPowerShrub),
    [FLDEFF_CUT_GRASS] = SCRIPT_PTR(ShowCutGrass),
    [FLDEFF_FIELD_MOVE_SHOW_MON_INIT] = SCRIPT_PTR(FieldMoveShowMonInit),
    [FLDEFF_USE_TOMB_PUZZLE_EFFECT] = SCRIPT_PTR(UsePuzzleEffect),
    [FLDEFF_PCTURN_ON] = SCRIPT_PTR(SecretBaseBootPC),
    [FLDEFF_HALL_OF_FAME_RECORD] = SCRIPT_PTR(HallOfFameRecord),
    [FLDEFF_USE_TELEPORT] = SCRIPT_PTR(UseTeleport),
    [FLDEFF_RAYQUAZA_SPOTLIGHT] = SCRIPT_PTR(RayquazaSpotlight),
    [FLDEFF_DESTROY_DEOXYS_ROCK] = SCRIPT_PTR(DestroyDeoxysRock),
    [FLDEFF_MOVE_DEOXYS_ROCK] = SCRIPT_PTR(MoveDeoxysRock),
};

#endif
