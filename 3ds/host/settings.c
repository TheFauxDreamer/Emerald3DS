// Port settings, persisted beside the save on the SD card.
//
// This is NOT save data and must never be treated like it. A missing, short,
// truncated or wrong-version file is an ordinary situation, not an error: the
// defaults apply and the game boots. Nothing here is allowed to block startup.
//
// Kept separate from save.c because the two have opposite requirements. The
// save image is 128 KB written on a debounce because Emerald writes a sector as
// thousands of single-byte programs; this is six bytes written when the player
// taps a button, which happens rarely enough that a debounce would be noise.
//
// The write-to-temp-then-rename discipline is borrowed from save.c all the same:
// an interrupted write leaves the previous settings intact rather than a
// half-written file that would then be discarded as corrupt.

#include <3ds.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../bridge.h"

#define SETTINGS_DIR  "sdmc:/3ds/emerald3ds"
#define SETTINGS_PATH SETTINGS_DIR "/settings.bin"

#define SETTINGS_MAGIC   0x53443345u   // 'E3DS' little-endian
// v3 changed what a binding VALUE means (CTR_BIND_MOD joined the speeds), so a
// v2 file must be discarded rather than reinterpreted: it would otherwise load
// as "nothing is the modifier" and silently lose the Y default.
//
// v4 appended the four gameplay tweaks. Nothing about the v3 fields changed, so
// unlike v2 a v3 file is MIGRATED rather than discarded: it is exactly the
// leading 12 bytes of a v4 file, and throwing it away would silently reset the
// player's top scale and turbo binds as the price of adding an unrelated
// option. The new fields take their defaults, which is what a v3 file means.
#define SETTINGS_VERSION 4

// Fixed-size, with every byte spoken for, so the on-disk layout does not depend
// on how the compiler chooses to align it.
struct CtrSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  topScale;
    // Was explicit padding, always written as 0. Claiming it costs no version
    // bump precisely because of that: every v3 file in existence has a zero
    // here, which is the same as the default this field wants.
    uint8_t  showAllTabs;
    uint8_t  turbo[CTR_TURBO_COUNT];   // CTR_BIND_OFF / a speed / CTR_BIND_MOD
    // Appended in v4. Everything above keeps its v3 offset, which is what makes
    // the migration below a plain short read rather than a conversion.
    uint8_t  expAll;
    uint8_t  levelCap;                 // CTR_CAP_*
    uint8_t  randomizer;
    uint8_t  bagSort;                  // CTR_BAGSORT_*
};

// How much of the struct a v3 file fills: everything up to the v4 additions.
#define SETTINGS_V3_SIZE  offsetof(struct CtrSettings, expAll)

// Defined in video.c and main.c, which own the live values.
extern int  Ctr3dsGetTopScale(void);
extern void Ctr3dsApplyTopScale(int mode);
extern int  Ctr3dsGetTurboBind(int button);
extern void Ctr3dsApplyTurboBind(int button, int value);
extern int  Ctr3dsGetShowAllTabs(void);
extern void Ctr3dsApplyShowAllTabs(int on);
extern int  Ctr3dsGetExpAll(void);
extern void Ctr3dsApplyExpAll(int on);
extern int  Ctr3dsGetLevelCap(void);
extern void Ctr3dsApplyLevelCap(int mode);
extern int  Ctr3dsGetRandomizer(void);
extern void Ctr3dsApplyRandomizer(int on);
extern int  Ctr3dsGetBagSort(void);
extern void Ctr3dsApplyBagSort(int mode);

void CtrSettingsLoad(void)
{
    struct CtrSettings s;

    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (f == NULL)
        return;                       // first run: defaults already in place

    // Zeroed first so a short v3 read leaves the v4 fields at their defaults
    // rather than at whatever was on the stack.
    memset(&s, 0, sizeof(s));

    size_t n = fread(&s, 1, sizeof(s), f);
    fclose(f);

    if (s.magic != SETTINGS_MAGIC)
        return;                       // anything unexpected: keep the defaults

    if (s.version == SETTINGS_VERSION)
    {
        if (n != sizeof(s))
            return;
    }
    else if (s.version == 3)
    {
        if (n != SETTINGS_V3_SIZE)
            return;                   // claims v3 but is not v3 shaped
    }
    else
    {
        return;                       // v2 or older, or from the future
    }

    // Range-check rather than trust the file: a value out of range would index
    // past the scale table in video.c.
    if (s.topScale < CTR_TOP_SCALE_COUNT)
        Ctr3dsApplyTopScale((int)s.topScale);

    // Any non-zero byte means on, so a corrupt value cannot be out of range.
    Ctr3dsApplyShowAllTabs(s.showAllTabs != 0);

    // Ctr3dsApplyTurboBind rejects anything that is not a valid binding, so a
    // corrupt byte leaves that button on its default rather than being trusted.
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        Ctr3dsApplyTurboBind(i, s.turbo[i]);

    // Zero for a migrated v3 file, which is the default for all four.
    // Ctr3dsApplyLevelCap and Ctr3dsApplyBagSort reject an out-of-range mode,
    // so a corrupt byte leaves that option off rather than being trusted.
    Ctr3dsApplyExpAll(s.expAll != 0);
    Ctr3dsApplyLevelCap(s.levelCap);
    Ctr3dsApplyRandomizer(s.randomizer != 0);
    Ctr3dsApplyBagSort(s.bagSort);
}

void CtrSettingsSave(void)
{
    struct CtrSettings s;

    s.magic    = SETTINGS_MAGIC;
    s.version  = SETTINGS_VERSION;
    s.topScale = (uint8_t)Ctr3dsGetTopScale();
    s.showAllTabs = (uint8_t)(Ctr3dsGetShowAllTabs() ? 1 : 0);
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        s.turbo[i] = (uint8_t)Ctr3dsGetTurboBind(i);
    s.expAll     = (uint8_t)(Ctr3dsGetExpAll() ? 1 : 0);
    s.levelCap   = (uint8_t)Ctr3dsGetLevelCap();
    s.randomizer = (uint8_t)(Ctr3dsGetRandomizer() ? 1 : 0);
    s.bagSort    = (uint8_t)Ctr3dsGetBagSort();

    mkdir("sdmc:/3ds", 0777);
    mkdir(SETTINGS_DIR, 0777);

    FILE *f = fopen(SETTINGS_PATH ".tmp", "wb");
    if (f == NULL)
        return;                       // read-only card, full card: not fatal

    size_t n = fwrite(&s, 1, sizeof(s), f);
    int flushed = (fflush(f) == 0);
    fclose(f);

    if (n != sizeof(s) || !flushed) {
        remove(SETTINGS_PATH ".tmp");
        return;
    }

    remove(SETTINGS_PATH);
    rename(SETTINGS_PATH ".tmp", SETTINGS_PATH);
}
