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
#define SETTINGS_VERSION 3

// Fixed-size and explicitly padded, so the on-disk layout does not depend on
// how the compiler chooses to align it.
struct CtrSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  topScale;
    uint8_t  reserved;
    uint8_t  turbo[CTR_TURBO_COUNT];   // CTR_BIND_OFF / a speed / CTR_BIND_MOD
};

// Defined in video.c and main.c, which own the live values.
extern int  Ctr3dsGetTopScale(void);
extern void Ctr3dsApplyTopScale(int mode);
extern int  Ctr3dsGetTurboBind(int button);
extern void Ctr3dsApplyTurboBind(int button, int value);

void CtrSettingsLoad(void)
{
    struct CtrSettings s;

    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (f == NULL)
        return;                       // first run: defaults already in place

    size_t n = fread(&s, 1, sizeof(s), f);
    fclose(f);

    if (n != sizeof(s) || s.magic != SETTINGS_MAGIC || s.version != SETTINGS_VERSION)
        return;                       // anything unexpected: keep the defaults

    // Range-check rather than trust the file: a value out of range would index
    // past the scale table in video.c.
    if (s.topScale < CTR_TOP_SCALE_COUNT)
        Ctr3dsApplyTopScale((int)s.topScale);

    // Ctr3dsApplyTurboBind rejects anything that is not a valid binding, so a
    // corrupt byte leaves that button on its default rather than being trusted.
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        Ctr3dsApplyTurboBind(i, s.turbo[i]);
}

void CtrSettingsSave(void)
{
    struct CtrSettings s;

    s.magic    = SETTINGS_MAGIC;
    s.version  = SETTINGS_VERSION;
    s.topScale = (uint8_t)Ctr3dsGetTopScale();
    s.reserved = 0;
    for (int i = 0; i < CTR_TURBO_COUNT; i++)
        s.turbo[i] = (uint8_t)Ctr3dsGetTurboBind(i);

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
