#!/bin/bash
# Build the pokeemerald game sources for the 3DS (ARM11, ARMv6K), and put them
# in the archive 3ds/build/libpokeemerald.a. This uses the same preproc pipeline
# as the WASM, modern and RP2350 builds. Run it from any directory: it changes
# to the repo root.
#
# This script comes from rp2350/build_objs.sh. There are two important
# differences:
# - The 3DS build sets -DRP2350=1 on purpose. In this tree, "RP2350" means "a
#   native CPU build, not GBA hardware". That gives no link cable, no real LCD
#   to follow VCOUNT, and no IWRAM mixer copy. Save writes go through the
#   Rp2350Save* hooks, and the Rp2350PresentFrame() callback runs on each frame.
#   The 3DS needs all of that, so the port uses it and does not copy 47 hook
#   sites. Then -DPLATFORM_3DS=1 changes only what is different on the 3DS: the
#   memory map (gba/defines.h) and the save storage (gba/flash_internal.h).
# - The ABI must be the same as the libctru ABI, or the link is wrong with no
#   error: armv6k, mpcore, hard float, soft thread pointer.
#
# The build needs pipefail. Each stage below is a pipeline that ends in the
# assembler, and an empty or truncated input is valid assembly. Without
# pipefail, a preproc that stops halfway (for example, at a missing .include)
# still gives exit status 0 and an incomplete object. The first sign is then an
# undefined reference at link time, which points at the wrong file.
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

# This must be the same as ARCH in 3ds/Makefile.
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations"

CFLAGS="$ARCH -O2 -ffreestanding -fno-strict-aliasing -fomit-frame-pointer \
  -ffunction-sections -fdata-sections \
  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-builtin-declaration-mismatch \
  -Wno-attributes -Wno-implicit-function-declaration -fno-common"

# Use -iquote, not -I. The string.h and strings.h in the game's include/ must
# apply only to "" includes, never to the libc <string.h>.
#
# CTR_BOOT_DIAG must be the same as in 3ds/Makefile. The boot traces in
# src/main.c are game-side. Without this flag, they compile to nothing, while
# the host-side traces still show. The build then has only half of its traces,
# and looks correct.
CTR_BOOT_DIAG="${CTR_BOOT_DIAG:-0}"

# The m4a mixer is the original src/m4a_1.s, not the C version.
#
# The file rp2350/m4a_engine.c exists because the Cortex-M33 of the RP2350 can
# execute only Thumb-2. It cannot execute ARMv4T ARM-mode code. The ARM11 is
# ARMv6K and can, so this port runs the assembly from Game Freak. That code
# cannot be wrong about the engine, as a new version can. It is the only
# configuration that this script builds. The RP2350 port still needs the C
# engine, so it stays in the tree.
CPPFLAGS="-iquote include -DMODERN=1 -DRP2350=1 -DPLATFORM_3DS=1 -DCTR_BOOT_DIAG=$CTR_BOOT_DIAG"

if [ ! -d "$ASSETS" ]; then
  echo "error: $ASSETS missing. Run 'make tools && make wasm-assets' first." >&2
  exit 1
fi

# The two preprocessing stages give no output, because the game's headers make
# much noise through cpp. That noise would hide the real compiler messages from
# the last stage. With pipefail on, a failure in a silent stage would stop the
# build with no output. Thus the function gives the name of the file that
# failed.
compile_c() {  # $1 = source .c, $2 = out .o
  $CC -E $CPPFLAGS "$1" 2>/dev/null \
    | $PP -i -g $ASSETS "$1" charmap.txt 2>/dev/null \
    | $CC -x c $CFLAGS -c - -o "$2" \
    || { echo "error: failed to build $1 (drop the 2>/dev/null in compile_c to see why)" >&2; return 1; }
}

# The data files also get -DPLATFORM_3DS. Thus a script can have a 3DS-only
# change behind #if PLATFORM_3DS, as the src/ hooks do. The event items use this
# (data/scripts/ctr3ds_event_tickets.inc and its four callers). No file that the
# data files include tests the flag, so nothing else in them changes. The builds
# of the top-level Makefile never define it.
assemble_s() { # $1 = data .s, $2 = out .o
  $PP "$1" charmap.txt \
    | $CC -E -DPLATFORM_3DS=1 -I include - \
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
# This does not compile rp2350/m4a_engine.c, on purpose. It and src/m4a_1.s
# define the same 31 symbols, so both together give a link error. This compiles
# m4a_mix.c, because it is the entry point of the host into the mixer, not the
# mixer.
#
# All objects go into one flat directory, and 3ds/ui/*.c comes last. Thus a
# 3ds/ui file with the same basename as a src file replaces that src object,
# and the game loses that feature with no error. Name 3ds/ui files ui_*, tab_*
# or view_*. Do not remove the prefixes. Subdirectories of 3ds/ui are not
# compiled.
for src in src/*.c rp2350/bios.c rp2350/asm_stubs.c rp2350/psg.c \
           rp2350/m4a_mix.c \
           3ds/gba_mem.c 3ds/tweaks.c 3ds/achievements.c 3ds/ui/*.c; do
  obj="$OBJ/$(basename "$src" .c).o"
  compile_c "$src" "$obj"
  n=$((n+1))
done
echo "      $n C objects"

echo "      m4a: assembling src/m4a_1.s (original mixer)"
# The -DPLATFORM_3DS flag moves SOUND_INFO_PTR and REG_BASE into gGbaMem. See
# the guard in constants/gba_constants.inc. The tools/preproc program expands
# the .include itself, so cpp evaluates that guard.
$PP src/m4a_1.s charmap.txt \
  | $CC -E -DPLATFORM_3DS=1 -I include -I . - \
  | $PP -ie src/m4a_1.s charmap.txt \
  | $AS -march=armv6k -mfloat-abi=hard -I include -I . -o "$OBJ/m4a_1.o" -
# SoundMainRAM_Buffer, the tail-jump target of m4a_1.s. The comment in
# 3ds/Makefile tells why neither .set nor --defsym can supply it.
$AS -march=armv6k -mfloat-abi=hard -o "$OBJ/m4a_arm11.o" 3ds/asm/m4a_arm11.s

echo "[2/3] assembling data sources..."
for s in maps map_events event_scripts battle_scripts_1 battle_scripts_2 battle_ai_scripts battle_anim_scripts; do
  assemble_s "data/$s.s" "$OBJ/data_$s.o"
done

echo "      sound: data + symbols + $(ls sound/songs/midi/*.s 2>/dev/null | wc -l | tr -d ' ') songs"
assemble_sound_data data/sound_data.s "$OBJ/data_sound_data.o"
# This does not assemble rp2350/sound_symbols.s, on purpose. That file defines
# gNumMusicPlayers and gMaxLines as absolute symbols, whose address is the
# value. The tool 3dsxtool cannot relocate these ("Relocation to invalid
# address!"). The 3DS build gets them as usual constants. See the PLATFORM_3DS
# branch in include/gba/m4a_internal.h.
ls sound/songs/midi/*.s | xargs -P 8 -I{} bash -c \
  "$AS -march=armv6k -mfloat-abi=hard -I sound -o \"$OBJ/song_\$(basename \"{}\" .s).o\" \"{}\""

echo "[3/3] archiving -> $OUT/libpokeemerald.a"
rm -f "$OUT/libpokeemerald.a"
$AR rcs "$OUT/libpokeemerald.a" "$OBJ"/*.o
echo "done: $(ls -la $OUT/libpokeemerald.a | awk '{print $5}') bytes, $(ls $OBJ/*.o | wc -l) objects"
