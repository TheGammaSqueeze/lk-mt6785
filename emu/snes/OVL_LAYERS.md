# DISP-OVL hardware layering for a 60fps idle home carousel

## Problem

The idle home carousel (state 0) is the only menu screen not at 60fps. Per-phase
telemetry (1280x960 panel, debug build): render 22.6ms, present 10.4ms -> 30fps.
Render is dominated by `draw_carousel` at 14.2ms: ~7 visible cards, each a large
scaled frame + boxart + player icon + 4 resume dots (~58 sprite/texture blits),
composited over the freshly-scrolled wallpaper EVERY frame. Everything else is
static when idle; only the wallpaper parallax legitimately changes per frame.

A pure-software card cache was tried (commit e787f6d) and reverted (cdbfc09): it
cached the cursorless card strip and CPU-composited it over the wallpaper each
frame. The carousel phase dropped 13.4->4.3ms, but the per-frame 3.7MB CPU
composite ballooned the OTHER phases +9ms (device memory-bandwidth/cache thrash,
invisible on the x86 host). Net zero, reverted.

## Key insight

The reverted cache did the hard part correctly: it SEPARATED the pulsing focus
cursor (`draw_focus_cursor`) from the static card bodies, built the bodies into an
alpha-preserving layer (`snes_target.cache_layer`), and gated rebuilds on a state
signature (`cc_signature`). The ONLY thing that failed was `composite_cardcache` -
the per-frame CPU blend. Hand that composite to the DISP-OVL hardware compositor
(proven by the bouncing-magenta-sprite milestone, commit e649ca7) and the CPU
never touches the cached buffer per frame. The exact bandwidth wall the revert hit
is removed: the OVL DMA reads the layers and alpha-blends them during scanout.

## Layering (state 0 home only; other states keep the single-buffer FB path,
which is already a solid 60fps)

Global OVL layer index == Z order. L0/L1 -> OVL0_2L (bottom), L2-L5 -> OVL0 (top).
Per-layer config is sticky: config each layer, then ONE `primary_display_trigger`.

- L0 (OVL0_2L, bottom) = the EXISTING framebuffer, double-buffered via s_fb_flip.
  Rendered every frame by `snes_menu_render`, but with the card bodies AND focus
  cursor SKIPPED. So L0 = wallpaper + chrome + filmstrip(thumbs+chevron) + hints +
  title + game name + everything else. Opaque. No new memory (reuses the FB).
- L2 (OVL0) = the cursorless card-body cache, STRAIGHT (coverage) alpha, rebuilt
  only when `cc_signature` changes (idle: never). New double buffer.
- L3 (OVL0) = the pulsing focus cursor, STRAIGHT alpha, rendered every frame into
  a small transparent buffer (cheap). New buffer.

Z-order is preserved by SPATIAL DISJOINTNESS: the card band (screen y ~222-498)
does not overlap chrome (top/bottom bars), title (y~152-178), filmstrip (y~535+),
or hints (y~604), so L2-on-top never wrongly occludes L0 content. The focus cursor
sits ON the focused card, so it must be above L2 -> L3.

## Alpha convention

MT6785 OVL blends with SURFL_EN=0 (only the RGBA4444 path sets it) = STRAIGHT /
coverage alpha: out = src.rgb*Aeff + dst.rgb*(1-Aeff), Aeff = per-pixel_a *
const_alpha(0xff). Confirmed by the sprite test: a straight buffer 0xB0FF00FF
(rgb FF00FF > a B0, invalid as premultiplied) composited as clean 69% magenta.
So L2/L3 hold STRAIGHT alpha. Build the cache in PREMULTIPLIED (clean painter's-
order source-over, reusing the validated cache_layer blit) then un-premultiply
once per build over the non-empty band (cheap: only AA-edge pixels differ from a=255).

## Correctness vs the reverted build_cardcache

The reverted `build_cardcache` rendered at offx=offy=0 with NO view transform and a
720-tall buffer. That is wrong in 4:3 (the direct render applies VIEW_CONTENT, a
1.185x zoom about the panel centre, with offy=0). Fix: build into a PANEL-SIZED
(1280x960) buffer using the SAME offx/offy and `set_view(VIEW_CONTENT)` as the live
render, so cached pixels are position-identical to direct in both 16:9 and 4:3. The
[cc_y0,cc_y1] non-empty band then limits the OVL src ROI (src_y/src_h, dst_y).

## Memory (must be in a MAPPED WB window; the OVL DMA reads DRAM)

CRITICAL: LK (k85v1_64) maps DRAM SELECTIVELY, not as a 1GB identity block. The
static platform.c mmu_initial_mappings has exactly three windows: mcusys
[0,0x40000000) STRONGLY_ORDERED (device, NOT dram); RAM WB [MEMBASE=0x4C400000,
+MEMSIZE=0x900000); and SCRATCH/download WB [SCRATCH_ADDR=0x4E000000,
+SCRATCH_SIZE=0x08000000) = [0x4E000000, 0x56000000). The FB is mblock-reserved
(which WB-maps it) near 0x80000000. EVERYTHING ELSE, including 0x56000000+
(LK_ADSP and up), is UNMAPPED - a CPU store there data-aborts (translation fault),
which is exactly what a first attempt at 0x57000000/0x58000000 did.

So the layer buffers MUST live in the SCRATCH WB window [0x4E000000, 0x56000000).
The existing SNES assets already do (BLOB 0x50000000 ... CHROME 0x53000000). We
reuse the two parked-bigcore slots (unused whenever AYANEO_BIGCORE_EXPT is off,
the default; the dead debug OVL sprite that also used 0x54000000 was removed):

- L0: existing FB (mblock, near 0x80000000), unchanged, s_fb_flip double buffer.
- L2 card cache: 0x54000000 double buffer (0x54000000 + 0x544B0000), 0x4B0000 each
  (was BC_COMMS). Flipped only on rebuild (idle: static, OVL keeps scanning live).
- L3 cursor: 0x55000000 double buffer (0x55000000 + 0x554B0000), 0x4B0000 each
  (was SPMFW staging). Flipped every frame (double-buffer avoids cursor tearing).
Both 0x960000 double buffers fit inside their 16MB slots, entirely below 0x56000000.

Each layer tracks its own live-buffer index. At present, config each layer's addr
= its live buffer, then one trigger. Flush (arch_clean_cache_range) a buffer after
CPU writes before the OVL reads it (once per build for L2; the dirty rect for L3).

## Validation (BEFORE any flash)

Extend host_render.c with a "layers" mode: render L0 (FB, cards+cursor skipped) +
L2 (cardcache) + L3 (cursor), software-composite them with the SAME straight-alpha
source-over the OVL uses, and byte-diff against the normal single-buffer render of
the identical state (16:9 and SNES_ASPECT43=1). Pixel-identity (modulo the cursor
colour-pulse phase, which is a single-frame timing artifact) proves the split is
output-correct. Only THEN wire the device present path and flash.

## Expected result

Idle: L0 render ~7.7ms (wallpaper 5.1 + chrome/filmstrip/hints/title ~2.6; cards
14ms GONE) + L3 cursor ~0.2ms; present ~7ms (clean one FB buffer + config 3
layers). ~15ms -> vsync-capped 60fps. Navigation still rebuilds L2 per frame
(~30fps during the brief scroll settle, same as today). Idle is where users dwell.
