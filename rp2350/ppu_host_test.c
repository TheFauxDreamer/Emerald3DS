// Host-side PPU validation. Loads a snapshot directory produced by
// tools/wasm_ppu_dump.mjs (reg/pal/vram/oam .bin + ref_rgba.bin), renders it
// with ppu.c, and pixel-diffs against the WASM reference.
//
//   cc -O2 -o /tmp/ppu_host_test rp2350/ppu_host_test.c rp2350/ppu.c
//   /tmp/ppu_host_test wasm-ppu-dump/000300-title
//
// Writes out.ppm (our render) and ref.ppm (reference) into the snapshot dir for
// eyeballing, and prints mismatch stats. Exit 0 if exact, 1 otherwise.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppu.h"

static uint8_t *load(const char *dir, const char *name, long expect, long *got) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (expect && size != expect) {
        fprintf(stderr, "%s: expected %ld bytes, got %ld\n", path, expect, size);
        exit(2);
    }
    uint8_t *buf = malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    if (got) *got = size;
    return buf;
}

static void write_ppm(const char *dir, const char *name, const uint8_t *rgb) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", PPU_WIDTH, PPU_HEIGHT);
    fwrite(rgb, 1, PPU_PIXELS * 3, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <snapshot-dir>\n", argv[0]); return 2; }
    const char *dir = argv[1];

    uint8_t *reg = load(dir, "reg.bin", 0x100, NULL);
    uint8_t *pal = load(dir, "pal.bin", 0x400, NULL);
    uint8_t *vram = load(dir, "vram.bin", 0x18000, NULL);
    uint8_t *oam = load(dir, "oam.bin", 0x400, NULL);
    uint8_t *ref = load(dir, "ref_rgba.bin", (long)PPU_PIXELS * 4, NULL);

    ppu_set_memory(reg, pal, vram, oam);

    static uint8_t img[PPU_PIXELS * 3];
    static uint8_t layer[PPU_PIXELS];
    ppu_render_rgb888(img, layer);

    // Also exercise the device path (renders straight to RGB565) and confirm it
    // tracks the byte-exact RGB888 path within the 565 round-trip rounding.
    // Compare in 565 space: differences come only from alpha-blend below-pixel
    // quantisation, expected to stay within a couple of LSBs of the packed ref.
    static uint16_t fb565[PPU_PIXELS];
    ppu_render_rgb565(fb565, layer);
    long worst565 = 0;
    for (int i = 0; i < PPU_PIXELS; i++) {
        uint16_t packed = (uint16_t)(((img[i * 3] & 0xf8) << 8)
                                     | ((img[i * 3 + 1] & 0xfc) << 3)
                                     | (img[i * 3 + 2] >> 3));
        int dr = abs(((fb565[i] >> 11) & 0x1f) - ((packed >> 11) & 0x1f));
        int dg = abs(((fb565[i] >> 5) & 0x3f) - ((packed >> 5) & 0x3f));
        int db = abs((fb565[i] & 0x1f) - (packed & 0x1f));
        int d = dr > dg ? dr : dg;
        if (db > d) d = db;
        if (d > worst565) worst565 = d;
    }

    // Build an RGB reference (drop alpha) for the PPM and the diff.
    static uint8_t refrgb[PPU_PIXELS * 3];
    for (int i = 0; i < PPU_PIXELS; i++) {
        refrgb[i * 3] = ref[i * 4];
        refrgb[i * 3 + 1] = ref[i * 4 + 1];
        refrgb[i * 3 + 2] = ref[i * 4 + 2];
    }

    long mismatch = 0, worst = 0, firstMismatch = -1;
    for (int i = 0; i < PPU_PIXELS; i++) {
        int dr = abs(img[i * 3] - refrgb[i * 3]);
        int dg = abs(img[i * 3 + 1] - refrgb[i * 3 + 1]);
        int db = abs(img[i * 3 + 2] - refrgb[i * 3 + 2]);
        int d = dr > dg ? dr : dg;
        if (db > d) d = db;
        if (d) {
            mismatch++;
            if (firstMismatch < 0) firstMismatch = i;
            if (d > worst) worst = d;
        }
    }

    write_ppm(dir, "out.ppm", img);
    write_ppm(dir, "ref.ppm", refrgb);

    printf("snapshot %s\n", dir);
    printf("  pixels     : %d\n", PPU_PIXELS);
    printf("  mismatched : %ld (%.3f%%)\n", mismatch, 100.0 * mismatch / PPU_PIXELS);
    printf("  worst chan : %ld\n", worst);
    printf("  565 vs 888 : worst %ld LSB (565 channel units)\n", worst565);
    if (firstMismatch >= 0)
        printf("  first diff : pixel %ld (x=%d, y=%d) ours=%d,%d,%d ref=%d,%d,%d\n",
               firstMismatch, (int)(firstMismatch % PPU_WIDTH), (int)(firstMismatch / PPU_WIDTH),
               img[firstMismatch * 3], img[firstMismatch * 3 + 1], img[firstMismatch * 3 + 2],
               refrgb[firstMismatch * 3], refrgb[firstMismatch * 3 + 1], refrgb[firstMismatch * 3 + 2]);
    printf("  wrote %s/out.ppm and ref.ppm\n", dir);
    return mismatch ? 1 : 0;
}
