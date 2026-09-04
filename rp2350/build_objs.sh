#!/bin/bash
# Build all pokeemerald game objects for RP2350 (Cortex-M33) and archive them
# into libpokeemerald.a, using the same preproc pipeline as the WASM/modern
# builds. Run from the repo root (pokeemerald-wasm/).
set -e
cd "$(dirname "$0")/.."   # repo root

OUT=rp2350/build
OBJ=$OUT/obj
mkdir -p "$OBJ"

CC=arm-none-eabi-gcc
PP=tools/preproc/preproc
ASSETS=build/assets
CFLAGS="-mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -O2 -ffreestanding \
  -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-builtin-declaration-mismatch \
  -Wno-attributes -Wno-implicit-function-declaration -fno-common"
CPPFLAGS="-iquote include -DMODERN=1 -DRP2350=1"

compile_c() {  # $1 = source .c, $2 = out .o
  $CC -E $CPPFLAGS "$1" 2>/dev/null \
    | $PP -i -g $ASSETS "$1" charmap.txt 2>/dev/null \
    | $CC -x c $CFLAGS -c - -o "$2"
}

assemble_s() { # $1 = data .s, $2 = out .o
  $PP "$1" charmap.txt 2>/dev/null \
    | $CC -E -I include - 2>/dev/null \
    | $PP -ie "$1" charmap.txt 2>/dev/null \
    | arm-none-eabi-as -mcpu=cortex-m33 -mthumb -I include -o "$2" -
}

# Sound data (m4a): voicegroups, samples (incbin .bin), song table + music
# player table -> needs the repo-root and sound/ include paths in addition to
# the preproc pipeline (incbin/charmap). Generate the .bin/.s assets first with
# rp2350/gen_sound_assets.sh.
assemble_sound_data() { # $1 = .s, $2 = out .o
  $PP "$1" charmap.txt 2>/dev/null \
    | $CC -E -I include -I . - 2>/dev/null \
    | $PP -ie "$1" charmap.txt 2>/dev/null \
    | arm-none-eabi-as -mcpu=cortex-m33 -mthumb -I . -I sound -I include -o "$2" -
}

echo "[1/3] compiling C sources..."
n=0
for src in src/*.c rp2350/bios.c rp2350/asm_stubs.c rp2350/m4a_1.c rp2350/psg.c; do
  obj="$OBJ/$(basename "$src" .c).o"
  compile_c "$src" "$obj"
  n=$((n+1))
done
echo "      $n C objects"

echo "[2/3] assembling data sources..."
for s in maps map_events event_scripts battle_scripts_1 battle_scripts_2 battle_ai_scripts battle_anim_scripts; do
  assemble_s "data/$s.s" "$OBJ/data_$s.o"
done

echo "      sound: data + symbols + $(ls sound/songs/midi/*.s 2>/dev/null | wc -l | tr -d ' ') songs"
assemble_sound_data data/sound_data.s "$OBJ/data_sound_data.o"
arm-none-eabi-as -mcpu=cortex-m33 -mthumb -o "$OBJ/sound_symbols.o" rp2350/sound_symbols.s
# Each song is its own object (defines the song-header symbol gSongTable points
# at). Plain gas with -I sound for the .include "MPlayDef.s"; parallelised.
ls sound/songs/midi/*.s | xargs -P 8 -I{} bash -c \
  'arm-none-eabi-as -mcpu=cortex-m33 -mthumb -I sound -o "'"$OBJ"'/song_$(basename "{}" .s).o" "{}"'

echo "[3/3] archiving -> $OUT/libpokeemerald.a"
rm -f "$OUT/libpokeemerald.a"
arm-none-eabi-ar rcs "$OUT/libpokeemerald.a" "$OBJ"/*.o
echo "done: $(ls -la $OUT/libpokeemerald.a | awk '{print $5}') bytes, $(ls $OBJ/*.o | wc -l) objects"
