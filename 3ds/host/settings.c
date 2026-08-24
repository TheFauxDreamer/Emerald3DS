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
#define SETTINGS_VERSION 1

// Fixed-size and explicitly padded, so the on-disk layout does not depend on
// how the compiler chooses to align it.
struct CtrSettings {
    uint32_t magic;
    uint16_t version;
    uint8_t  topScale;
    uint8_t  reserved;
};

// Defined in video.c, which owns the live value.
extern int  Ctr3dsGetTopScale(void);
extern void Ctr3dsApplyTopScale(int mode);

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
}

void CtrSettingsSave(void)
{
    struct CtrSettings s;

    s.magic    = SETTINGS_MAGIC;
    s.version  = SETTINGS_VERSION;
    s.topScale = (uint8_t)Ctr3dsGetTopScale();
    s.reserved = 0;

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
