#!/bin/bash
# Generate the m4a sound assets the RP2350 archive links (build_objs.sh expects
# them present): sample .bin from .wav (wav2agb) and song .s from .mid (mid2agb).
# One-time / regenerate after changing sound sources. Run from the repo root.
#
# Mirrors audio_rules.mk:
#   cries:  wav2agb -b -c -l 1 --no-pad   ;  other samples: wav2agb -b
#   songs:  mid2agb in.mid out.s <opts-from-sound/songs/midi/midi.cfg>
set -e
cd "$(dirname "$0")/.."   # repo root

WAV2AGB=tools/wav2agb/wav2agb
MID=tools/mid2agb/mid2agb
MIDDIR=sound/songs/midi

echo "[sound] samples: $(find sound -name '*.wav' | wc -l | tr -d ' ') wav -> bin"
gen_sample() {
  local wav="$1" bin="${1%.wav}.bin"
  if [[ "$wav" == *"/cries/"* ]]; then
    "$WAV2AGB" -b -c -l 1 --no-pad "$wav" "$bin"
  else
    "$WAV2AGB" -b "$wav" "$bin"
  fi
}
export -f gen_sample
export WAV2AGB
find sound -name '*.wav' -print0 | xargs -0 -P 8 -I{} bash -c 'gen_sample "$@"' _ {}

echo "[sound] songs: mid2agb per $MIDDIR/midi.cfg"
fail=0
while IFS= read -r line; do
  [[ -z "$line" || "$line" == \#* ]] && continue
  midfile="${line%%:*}"
  opts="${line#*:}"
  base="${midfile%.mid}"   # some cfg lines omit the .mid extension
  [[ -f "$MIDDIR/$base.mid" ]] || continue
  "$MID" "$MIDDIR/$midfile" "$MIDDIR/$base.s" $opts >/dev/null 2>&1 || { echo "  FAIL $base"; fail=$((fail+1)); }
done < "$MIDDIR/midi.cfg"

echo "[sound] done: $(find sound -name '*.bin' | wc -l | tr -d ' ') bin, $(ls $MIDDIR/*.s 2>/dev/null | wc -l | tr -d ' ') song .s, song failures=$fail"
