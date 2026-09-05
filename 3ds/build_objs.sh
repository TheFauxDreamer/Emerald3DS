#!/bin/bash
# Build the pokeemerald game sources for the 3DS (ARM11 / ARMv6K) and archive
# them into 3ds/build/libpokeemerald.a, using the same preproc pipeline as the
# WASM/modern/RP2350 builds. Run from anywhere; it cds to the repo root.
#
# Adapted from rp2350/build_objs.sh. Two differences that matter:
#
#   -DRP2350=1 is deliberate on a 3DS. In this tree "RP2350" has come to mean
#   "native CPU build, not GBA hardware": no link cable, no real LCD to chase
#   VCOUNT, no IWRAM mixer copy, save writes through the Rp2350Save* hooks, and
#   the per-frame Rp2350PresentFrame() callback. All of that is exactly what the
#   3DS wants, so the port inherits it instead of duplicating 47 seam sites.
#   -DPLATFORM_3DS=1 then overrides only where the 3DS genuinely differs
#   (gba/defines.h memory map, gba/flash_internal.h save backing).
#
#   The ABI must match libctru exactly or the link is silently wrong:
#   armv6k / mpcore / hard-float / soft thread-pointer.
#
# pipefail is load-bearing. Every stage below is a pipeline ending in the
# assembler, and an empty or truncated input is perfectly valid assembly.
# Without pipefail a preproc that dies halfway through (a missing .include, say)
# still yields a 0 exit and a silently incomplete object; the first sign of
# trouble is then an undefined reference at link time, pointing at the wrong file.
set -eo pipefail
cd "$(dirname "$0")/.."   # repo root

OUT=3ds/build
OBJ=$OUT/obj
mkdir -p "$OBJ"

if ! command -v arm-none-eabi-gcc >/dev/null; then
  echo "error: arm-none-eabi-gcc not found. Install devkitPro (devkitARM) and" >&2
  echo "       open a new shell so /opt/devkitpro/devkitARM/bin is on PATH." >&2
  exit 1
fi

CC=arm-none-eabi-gcc
AS=arm-none-eabi-as
AR=arm-none-eabi-ar
PP=tools/preproc/preproc
ASSETS=build/assets

# Must match 3ds/Makefile's ARCH exactly.
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations"

CFLAGS="$ARCH -O2 -ffreestanding -fno-strict-aliasing -fomit-frame-pointer \
  -ffunction-sections -fdata-sections \
  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-builtin-declaration-mismatch \
  -Wno-attributes -Wno-implicit-function-declaration -fno-common"

# -iquote, not -I: the game's include/ has string.h/strings.h that must only
# satisfy "" includes, never hijack libc's <string.h>.
# CTR_BOOT_DIAG must match 3ds/Makefile: the bring-up traces in src/main.c are
# game-side, so without it here they silently compile to nothing while the
# host-side traces still appear -- a half-instrumented build that looks fine.
CTR_BOOT_DIAG="${CTR_BOOT_DIAG:-0}"

# Run the ORIGINAL m4a mixer (src/m4a_1.s) instead of the C reimplementation.
#
# The C port exists because the RP2350's Cortex-M33 is Thumb-2 only and cannot
# execute ARMv4T ARM-mode code. The ARM11 is ARMv6K and can, so on this target
# the assembly is available -- and being the actual shipped code, it cannot be
# wrong about the engine the way a reimplementation can.
#
# Off by default until it has been heard on hardware. Build with
# `CTR_M4A_ASM=1 3ds/build_objs.sh` to try it; everything else is unchanged, so
# a bad result costs one build rather than the branch.
CTR_M4A_ASM="${CTR_M4A_ASM:-0}"

CPPFLAGS="-iquote include -DMODERN=1 -DRP2350=1 -DPLATFORM_3DS=1 -DCTR_BOOT_DIAG=$CTR_BOOT_DIAG -DCTR_M4A_ASM=$CTR_M4A_ASM"

if [ ! -d "$ASSETS" ]; then
  echo "error: $ASSETS missing. Run 'make tools && make wasm-assets' first." >&2
  exit 1
fi

# The two preprocessing stages stay muted: the game's headers are noisy enough
# through cpp to bury the real compiler diagnostics from the final stage. With
# pipefail on, a failure in a muted stage would otherwise abort with no output
# at all, so name the file that broke.
compile_c() {  # $1 = source .c, $2 = out .o
  $CC -E $CPPFLAGS "$1" 2>/dev/null \
    | $PP -i -g $ASSETS "$1" charmap.txt 2>/dev/null \
    | $CC -x c $CFLAGS -c - -o "$2" \
    || { echo "error: failed to build $1 (drop the 2>/dev/null in compile_c to see why)" >&2; return 1; }
}

assemble_s() { # $1 = data .s, $2 = out .o
  $PP "$1" charmap.txt \
    | $CC -E -I include - \
    | $PP -ie "$1" charmap.txt \
    | $AS -march=armv6k -mfloat-abi=hard -I include -o "$2" -
}

assemble_sound_data() { # $1 = .s, $2 = out .o
  $PP "$1" charmap.txt \
    | $CC -E -I include -I . - \
    | $PP -ie "$1" charmap.txt \
    | $AS -march=armv6k -mfloat-abi=hard -I . -I sound -I include -o "$2" -
}

echo "[1/3] compiling C sources..."
n=0
# m4a_engine.c is the C reimplementation of src/m4a_1.s. It is dropped when the
# assembly is used, or the two define the same 31 symbols and the link fails.
M4A_ENGINE=rp2350/m4a_engine.c
if [ "$CTR_M4A_ASM" = "1" ]; then
  M4A_ENGINE=
fi

for src in src/*.c rp2350/bios.c rp2350/asm_stubs.c rp2350/psg.c \
           $M4A_ENGINE rp2350/m4a_mix.c \
           3ds/gba_mem.c 3ds/tweaks.c 3ds/ui/*.c; do
  obj="$OBJ/$(basename "$src" .c).o"
  compile_c "$src" "$obj"
  n=$((n+1))
done
echo "      $n C objects"

if [ "$CTR_M4A_ASM" = "1" ]; then
  echo "      m4a: assembling src/m4a_1.s (original mixer)"
  # -DPLATFORM_3DS is what redirects SOUND_INFO_PTR and REG_BASE into gGbaMem;
  # see the guard in constants/gba_constants.inc. tools/preproc expands the
  # .include itself, so cpp is what evaluates that guard.
  $PP src/m4a_1.s charmap.txt \
    | $CC -E -DPLATFORM_3DS=1 -I include -I . - \
    | $PP -ie src/m4a_1.s charmap.txt \
    | $AS -march=armv6k -mfloat-abi=hard -I include -I . -o "$OBJ/m4a_1.o" -
fi

echo "[2/3] assembling data sources..."
for s in maps map_events event_scripts battle_scripts_1 battle_scripts_2 battle_ai_scripts battle_anim_scripts; do
  assemble_s "data/$s.s" "$OBJ/data_$s.o"
done

echo "      sound: data + symbols + $(ls sound/songs/midi/*.s 2>/dev/null | wc -l | tr -d ' ') songs"
assemble_sound_data data/sound_data.s "$OBJ/data_sound_data.o"
# rp2350/sound_symbols.s is deliberately NOT assembled here. It defines
# gNumMusicPlayers/gMaxLines as absolute symbols whose address is the value,
# which 3dsxtool cannot relocate ("Relocation to invalid address!"). The 3DS
# build gets them as plain constants instead -- see the PLATFORM_3DS branch in
# include/gba/m4a_internal.h.
ls sound/songs/midi/*.s | xargs -P 8 -I{} bash -c \
  "$AS -march=armv6k -mfloat-abi=hard -I sound -o \"$OBJ/song_\$(basename \"{}\" .s).o\" \"{}\""

# Record which mixer went into the archive, so 3ds/Makefile reports the truth
# rather than whatever CTR_M4A_ASM happened to be set to when make was invoked.
# The two are separate invocations and it is easy to pass the flag to one only.
echo "$CTR_M4A_ASM" > "$OUT/m4a_asm.flag"

echo "[3/3] archiving -> $OUT/libpokeemerald.a"
rm -f "$OUT/libpokeemerald.a"
$AR rcs "$OUT/libpokeemerald.a" "$OBJ"/*.o
echo "done: $(ls -la $OUT/libpokeemerald.a | awk '{print $5}') bytes, $(ls $OBJ/*.o | wc -l) objects"
