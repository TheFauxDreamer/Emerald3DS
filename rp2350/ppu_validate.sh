#!/usr/bin/env bash
# Headless PPU validation: dump GBA memory + reference frames from the WASM build,
# render each with the C PPU (ppu.c), and pixel-diff against the reference.
#
#   rp2350/ppu_validate.sh [events.txt] [dump-dir]
#
# Defaults to the full intro->mudkip-battle route. Requires Chrome and a prebuilt
# build/wasm/pokeemerald.wasm (uses --no-build; run `make wasm` first to refresh).
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root (pokeemerald-wasm)

EVENTS="${1:-tools/wasm_replays/mudkip_starter.txt}"
DUMP="${2:-/tmp/ppu_validate}"
BIN=/tmp/ppu_host_test

echo "==> building host PPU test"
cc -O2 -Wall -Wextra -o "$BIN" rp2350/ppu_host_test.c rp2350/ppu.c

echo "==> dumping snapshots from WASM ($EVENTS)"
node tools/wasm_ppu_dump.mjs "$EVENTS" "$DUMP" --no-build >/dev/null

echo "==> diffing C PPU against reference"
pass=0; fail=0
for d in "$DUMP"/0*/; do
  out="$("$BIN" "$d" 2>&1)"
  mm="$(echo "$out" | awk '/mismatched/{print $3}')"
  name="$(basename "$d")"
  if [ "$mm" = "0" ]; then
    pass=$((pass+1)); printf '  ok   %s\n' "$name"
  else
    fail=$((fail+1)); printf '  FAIL %s (%s mismatched)\n' "$name" "$mm"
  fi
done
echo "==> PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
