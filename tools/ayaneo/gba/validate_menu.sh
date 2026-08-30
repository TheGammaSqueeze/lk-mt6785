#!/bin/bash
#
# Render every GBA-menu state from a packed blob and verify each one rendered a
# non-blank frame. Catches engine/asset regressions across ALL states (the build
# smoke test only exercises one nav path). Optionally writes PNGs for visual review.
#
# Usage:
#   tools/ayaneo/gba/validate_menu.sh [snes_pack.bin] [--png OUTDIR]
# Default pack: out/snes_pack.bin. Exits non-zero if any state is blank / fails.
#
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
PACK="${1:-out/snes_pack.bin}"
PNGDIR=""
[ "${2:-}" = "--png" ] && PNGDIR="${3:-/tmp/gbamenu_states}"

[ -f "$PACK" ] || { echo "!! pack not found: $PACK" >&2; exit 2; }

echo ">> building host renderer"
bash emu/gba/menu/build_host.sh >/dev/null 2>&1 || { echo "!! host build failed" >&2; exit 2; }
HR=emu/gba/menu/host_render
[ -n "$PNGDIR" ] && mkdir -p "$PNGDIR"

# state name -> nav string (settle-per-key harness; see host_render.c)
STATES=(
  "home:"
  "carousel_scrolled:RRR"
  "sorted:S"
  "menubar_display:U"
  "menubar_manual:URRRR"
  "sub_display:URA"
  "sub_options:URRA"
  "sub_language:URRRA"
  "sub_copyright:URRRRA"
  "sub_manual:URRRRRA"
  "resume:D"
)

blank_check() {   # $1 = ppm ; exit 0 if non-blank (>1% non-zero pixel bytes)
  python3 -c "import sys
d=open('$1','rb').read();i=0
for _ in range(3): i=d.index(b'\n',i)+1
b=d[i:];sys.exit(0 if sum(x!=0 for x in b)>len(b)//100 else 1)"
}

# large-library coverage (roster:name:nav): the shoulder page-jump ([ / ]), the
# scroll-position bar, the scrolling filmstrip and sort-at-scale only exist for big
# rosters, which the fixed n=6 pass above cannot exercise (the old 8-cap once hid a
# real filmstrip-cram bug). '[' = L-shoulder page back, ']' = R-shoulder page forward.
LARGE_STATES=(
  "10:ring10_jump:]"
  "40:big_home:"
  "40:big_jump_fwd:]]"
  "40:big_jump_back:]][["
  "40:big_sorted:]]S"
  "128:huge_home:"
  "128:huge_jump:]]]]]]"
)

fail=0
for entry in "${STATES[@]}"; do
  name="${entry%%:*}"; nav="${entry#*:}"
  ppm="/tmp/gbamenu_${name}.ppm"
  if GBA_ROSTER=6 "$HR" "$PACK" "$ppm" 60 "$nav" >/dev/null 2>&1 && blank_check "$ppm"; then
    printf "  OK   %-20s (nav '%s')\n" "$name" "$nav"
    if [ -n "$PNGDIR" ]; then
      python3 -c "from PIL import Image; Image.open('$ppm').save('$PNGDIR/$name.png')" 2>/dev/null
    fi
  else
    printf "  FAIL %-20s (nav '%s') - blank or crashed\n" "$name" "$nav"
    fail=1
  fi
done

for entry in "${LARGE_STATES[@]}"; do
  roster="${entry%%:*}"; rest="${entry#*:}"; name="${rest%%:*}"; nav="${rest#*:}"
  ppm="/tmp/gbamenu_${name}.ppm"
  if GBA_ROSTER="$roster" "$HR" "$PACK" "$ppm" 30 "$nav" >/dev/null 2>&1 && blank_check "$ppm"; then
    printf "  OK   %-20s (n=%s nav '%s')\n" "$name" "$roster" "$nav"
    if [ -n "$PNGDIR" ]; then
      python3 -c "from PIL import Image; Image.open('$ppm').save('$PNGDIR/$name.png')" 2>/dev/null
    fi
  else
    printf "  FAIL %-20s (n=%s nav '%s') - blank or crashed\n" "$name" "$roster" "$nav"
    fail=1
  fi
done

if [ "$fail" = 0 ]; then
  echo ">> all $(( ${#STATES[@]} + ${#LARGE_STATES[@]} )) states rendered non-blank"
  [ -n "$PNGDIR" ] && echo "   PNGs in $PNGDIR"
else
  echo "!! one or more states failed" >&2
fi
exit $fail
