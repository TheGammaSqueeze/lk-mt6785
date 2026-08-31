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
  # adversarial stress: a long chaotic sequence (menubar in/out, submenus, resume,
  # sorts, shoulder jumps, launches, back) must never crash/hang and must end in a
  # valid non-blank frame - exercises the state machine the single-path states cannot.
  "1:chaos_n1:URDLRUDABURRRAB]S[]S]]DUURRRRAB[[]SSUDRLRL]]]]SABUDR]["
  "8:chaos_n8:URDLRUDABURRRAB]S[]S]]DUURRRRAB[[]SSUDRLRL]]]]SABUDR]["
  "40:chaos_n40:URDLRUDABURRRAB]S[]S]]DUURRRRAB[[]SSUDRLRL]]]]SABUDR]["
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

# audio assets: the BGM + SFX GUIDs the menu wires must resolve in the pack (rendering
# never touches audio, so a missing sound would otherwise only surface on device).
if AUDIT_AUDIO=1 "$HR" "$PACK" /tmp/gbamenu_audit.ppm >/tmp/gbamenu_audio.txt 2>&1; then
  sed 's/^/  /' /tmp/gbamenu_audio.txt
else
  echo "  FAIL audio - one or more BGM/SFX assets MISSING from the pack" >&2
  sed 's/^/  /' /tmp/gbamenu_audio.txt
  fail=1
fi

# never-brick fallback: if the SNES pack is ever missing/corrupt the menu falls back to
# the plain-list selector (rs_move/rs_scroll). That path is safety-critical, so run its
# standalone host test here too (it lives otherwise only in the heavier FAT/SD suite).
if gcc -O2 -Iemu/gba -o /tmp/gbamenu_nav_test emu/gba/rom_select_nav_test.c >/dev/null 2>&1 \
   && /tmp/gbamenu_nav_test >/dev/null 2>&1; then
  echo "  OK   never-brick fallback nav (rs_move/rs_scroll)"
else
  echo "  FAIL never-brick fallback nav test" >&2; fail=1
fi

# punch-hole launch transition: the compositor (gba_punch_composite) runs only on
# device, so host-test its geometry (game inside the growing circle, black letterbox,
# menu snapshot outside) via the standalone test.
if gcc -O2 -Iemu/gba/menu -o /tmp/gbamenu_punch_test emu/gba/gba_punch_test.c >/dev/null 2>&1 \
   && /tmp/gbamenu_punch_test >/dev/null 2>&1; then
  echo "  OK   punch-hole transition compositor"
else
  echo "  FAIL punch-hole transition compositor" >&2; fail=1
fi

# launch correctness: a nav ending in A must hand back order[focus] (the right ROM),
# which must hold even after a SELECT sort reverses the order (the SA case: sort then
# launch the on-screen game must return its real SD index, not a screen position).
for lnav in A RRRA "]]A" SA "]]SA" RRSA "[A"; do
  if out=$(GBA_ROSTER=40 AUDIT_LAUNCH=1 "$HR" "$PACK" /tmp/gbamenu_launch.ppm 20 "$lnav" 2>&1); then
    printf "  OK   launch %-8s %s\n" "$lnav" "$(echo "$out" | grep -o 'launch=[0-9]* .*OK')"
  else
    printf "  FAIL launch %-8s - wrong ROM index\n" "$lnav"; echo "$out" | sed 's/^/    /'
    fail=1
  fi
done

# NEON blitter parity: the DEVICE runs the snes_render.c __ARM_NEON path, but the x86
# host build above only compiles the scalar fallback, so a NEON-specific pixel bug would
# ship untested. When an ARM cross-gcc + qemu-arm-static are present, build the ARM+NEON
# harness and diff its output vs the scalar one (must be pixel-identical). Skipped (not
# failed) when the toolchain is absent.
if command -v qemu-arm-static >/dev/null 2>&1 && command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
  if bash emu/gba/menu/build_host_arm.sh >/tmp/gbamenu_arm_build.txt 2>&1; then
    HRA=emu/gba/menu/host_render_arm; neonfail=0
    # cover both NEON blocks (snes_render.c:113 sprite/text 565->8888, :751 cached-tile
    # blit_raw) and their opaque/transparent/mixed-edge branches: home + carousel exercise
    # the tile blit, the submenu + menubar exercise varied sprites/glyphs, GBA carts the
    # cached path, resume the dimmed (scalar) cached blit.
    for pair in "0:" "0:RRR" "0:URRRRA" "0:U" "40:" "40:]]S" "40:D"; do
      ro="${pair%%:*}"; nv="${pair#*:}"
      GBA_ROSTER="$ro" "$HR"  "$PACK" /tmp/gbamenu_sc.ppm 30 "$nv" >/dev/null 2>&1
      GBA_ROSTER="$ro" qemu-arm-static "$HRA" "$PACK" /tmp/gbamenu_ne.ppm 30 "$nv" >/dev/null 2>&1
      if ! cmp -s /tmp/gbamenu_sc.ppm /tmp/gbamenu_ne.ppm; then
        printf "  FAIL neon   n=%s nav '%s' - NEON output differs from scalar\n" "$ro" "$nv"; neonfail=1; fail=1
      fi
    done
    [ "$neonfail" = 0 ] && echo "  OK   neon blitter parity (ARM+NEON == scalar, 7 states, both blit paths)"
    # ARM logic parity: the nav/sort/launch integer logic runs on the device (ARM); a
    # sign/width bug there would boot the wrong game. Confirm the launched index matches
    # x86 across sort/jump combos (the render diff above only covers pixels, not logic).
    logicfail=0
    for lnav in A "]]A" SA "]]SA" RRSA "[A"; do
      lx=$(GBA_ROSTER=40 AUDIT_LAUNCH=1 "$HR"  "$PACK" /tmp/gbamenu_lx.ppm 20 "$lnav" 2>&1 | grep -o 'launch=[0-9-]*')
      la=$(GBA_ROSTER=40 AUDIT_LAUNCH=1 qemu-arm-static "$HRA" "$PACK" /tmp/gbamenu_la.ppm 20 "$lnav" 2>&1 | grep -o 'launch=[0-9-]*')
      [ "$lx" = "$la" ] || { printf "  FAIL arm-logic %-6s x86=%s arm=%s\n" "$lnav" "$lx" "$la"; logicfail=1; fail=1; }
    done
    [ "$logicfail" = 0 ] && echo "  OK   arm logic parity (nav/sort/launch ARM == x86, 6 cases)"
  else
    echo "  skip neon - ARM host build failed (see /tmp/gbamenu_arm_build.txt)"
  fi
else
  echo "  skip neon - no arm-linux-gnueabihf-gcc + qemu-arm-static (device NEON path unchecked here)"
fi

if [ "$fail" = 0 ]; then
  echo ">> all $(( ${#STATES[@]} + ${#LARGE_STATES[@]} )) states rendered non-blank + audio + launch OK"
  [ -n "$PNGDIR" ] && echo "   PNGs in $PNGDIR"
else
  echo "!! one or more states failed" >&2
fi
exit $fail
