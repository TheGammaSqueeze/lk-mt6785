# Dynamic Per-Core LCM Refresh (with real-time region switching)

## 1. Summary

This plan makes the st7703_hd720 DSI panel follow each libretro core's requested frame rate, including the PAL rates around 50 Hz, and makes region changes take effect in real time while a game is running. Today every core pins the panel to a fixed NTSC vfp at launch (Genesis GEN_VFP=20, snes9x SNES_VFP=17) and the Genesis GM_REGION menu handler changes the emulated region but never re-applies the panel vfp, so switching a Genesis game to Europe mid-session leaves the panel at ~59.9 Hz while the core runs a 49.7 Hz PAL frame, which because the present is vsync-locked (one emulated frame per panel refresh) makes the game run about 20.5% fast with pitched-up audio. The headline change is the real-time region switch: the Genesis GM_REGION handler arms a deferred re-tune that the session loop consumes right after the committed frame that applies the region, so the panel refresh moves at the moment you toggle the region. Supporting that: the DSI vfp software clamp is raised from 200 (a 50.75 Hz floor) so PAL vfp targets are reachable, a fps-milliHz to vfp helper is added in the DSI layer, and both the Genesis and snes9x cores gain a trailing fps_milli() ABI export (with the full version-literal lockstep across the ABI header, the exports table, and the blob .ld) so LK can poll the core's live frame rate. The Genesis/SNES audio path needs no change because its resampler self-adapts to whatever rate the panel paces; the fast-forward budget rescale is a completeness item, not a correctness fix.

Canonical line constant and vfp targets used throughout this plan (shipped build defines AYANEO_GBA, so AYANEO_DSI_LINE_CONST = 59667000):

| Core / region | fps_milli | vtotal | vfp | delivered |
|---|---|---|---|---|
| Genesis NTSC | 59923 | 996 | 20 | 59.907 Hz |
| Genesis PAL | 49702 | 1200 | 224 | 49.722 Hz |
| snes9x NTSC | 60099 | 993 | 17 | 60.088 Hz |
| snes9x PAL | 50007 | 1193 | 217 | 50.014 Hz |
| GBA/GBC (fixed) | 59727 | 999 | 23 | 59.727 Hz |

Non-shipped build (if AYANEO_GBA is ever undefined, constant 59684000) yields the same NTSC/GBA vfp but PAL Genesis vfp 225 and PAL SNES vfp 218 (worst-case vtotal 1201). NTSC and GBA targets are identical under both constants; PAL differs by exactly one line, a real ~0.03 Hz shift, which is why the constant is gated on AYANEO_GBA rather than treated as cosmetic.

## 2. Change set in dependency order

The order below is the required build/author order. Group A (the DSI mechanism) is the foundation and must exist before the harness callers link. Groups B and C (Genesis and snes9x ABI + harness) are independent of each other but each must ship as one atomic blob+lk build because of the ABI version lockstep. Group D (audio evidence + FF budget) is a leaf.

### Group A: DSI vfp mechanism (LK image, no ABI/version/.ld impact)

These are plain C functions in platform/mt6785. They are compiled into the LK image, not into any core blob, so none of the GENESIS_CORE_ABI_VERSION / .ld LONG() / loaderr machinery applies here.

**A1. `/work/svoboda_lk/platform/mt6785/ddp_dsi.c` - add the line-constant macro (after line 55, before the include on line 57).**

Current (lines 53-55):
```c
#if defined(AYANEO_GBA)
#define AYANEO_GBA_DR_KHZ 526512u
#endif
```

Insert immediately after line 55:
```c
/* AYANEO: panel line rate numerator (milliHz-per-line * total lines) used to map refresh<->vfp:
 * refresh_milliHz = AYANEO_DSI_LINE_CONST / vtotal, vtotal = 976 + vfp. The literal 59684000 was
 * calibrated on the STOCK integer data rate. The shipped build defines AYANEO_GBA, which overrides
 * the MIPI data rate to AYANEO_GBA_DR_KHZ=526512 (see the DSI_*PHY_clk_setting PCW blocks) and
 * disables SSC, lowering the true line rate to ~59.667 kHz. Gate the constant on the SAME define
 * so read and write agree. This is NOT cosmetic for PAL: under 59667000 the PAL vfp targets are
 * 217 (SNES) / 224 (Genesis); under 59684000 they are 218 / 225 (a one-line, ~0.03 Hz shift).
 * NTSC (20/17) and GBA (23) are identical under both constants. */
#if defined(AYANEO_GBA) && AYANEO_GBA_DR_KHZ
#define AYANEO_DSI_LINE_CONST 59667000u
#else
#define AYANEO_DSI_LINE_CONST 59684000u
#endif
```
The gate `#if defined(AYANEO_GBA) && AYANEO_GBA_DR_KHZ` matches the existing PLL-override gate at ddp_dsi.c:1618. AYANEO_GBA is defined for the shipped image via k85v1_64.mk:77/91.

**A2. `/work/svoboda_lk/platform/mt6785/ddp_dsi.c` - raise the vfp upper clamp (line 1104 only).**

Current (lines 1101-1106):
```c
void ayaneo_dsi_set_vfp(unsigned int vfp)
{
	if (vfp < 4u)   vfp = 4u;
	if (vfp > 200u) vfp = 200u;
	DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL, vfp);
}
```

Change only line 1104:
```c
void ayaneo_dsi_set_vfp(unsigned int vfp)
{
	if (vfp < 4u)   vfp = 4u;
	if (vfp > 256u) vfp = 256u;   /* was 200 (50.75Hz floor). 256 -> vtotal 1232 -> ~48.4Hz floor,
	                                 clears worst-case PAL Genesis vfp 225 (non-shipped const) with margin */
	DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL, vfp);
}
```
Leave the lower clamp (4u) and the DSI_OUTREG32 poke exactly as they are. The register field DSI_VFP_NL_REG.VFP_NL is 12-bit (ddp_reg_dsi.h ~499-502, max 4095), so 256 is a software safety limit well inside hardware range. Do NOT raise to 4095: st7703 electrical stability at very high vtotal is the biggest untestable risk. Shipped-build worst case is only vfp 224; 256 gives headroom without entering an untested regime.

**A3. `/work/svoboda_lk/platform/mt6785/ddp_dsi.c` - new helper `ayaneo_dsi_set_fps_milli`, inserted between the close of `ayaneo_dsi_refresh_milli` (line 1126) and `DSI_Config_VDO_Timing` (line 1128).**

```c
/* AYANEO: pick the vfp that lands the panel refresh on a target fps (milli-Hz) and apply it live.
 * vtotal = round(AYANEO_DSI_LINE_CONST / fps_milli); vfp = vtotal - 976 (non-vfp lines sum to 976 on
 * this st7703_hd720 timing). Rounding is (const + fps/2) / fps. The [4,256] clamp and the single
 * register write live in ayaneo_dsi_set_vfp(), which this calls, so the poke is never duplicated.
 * Real-time safe: in continuous VDO mode DSI_VFP_NL is re-read at the next frame boundary, no DSI
 * reset. Do NOT wait_for_vsync here (double-wait = 30fps). Returns the pre-clamp vfp so callers can
 * log it. Guards fps_milli==0. */
unsigned int ayaneo_dsi_set_fps_milli(unsigned int fps_milli)
{
	unsigned int line_const = AYANEO_DSI_LINE_CONST;
	unsigned int vtotal, vfp;
	if (fps_milli == 0u) return 0u;
	vtotal = (line_const + fps_milli / 2u) / fps_milli;   /* rounded */
	if (vtotal < 976u + 4u) vtotal = 976u + 4u;           /* keep vfp subtraction non-negative */
	vfp = vtotal - 976u;
	ayaneo_dsi_set_vfp(vfp);   /* single write + [4,256] clamp */
	return vfp;
}
```
Worked targets under the shipped 59667000: fps_milli=50007 (PAL SNES) -> vtotal=(59667000+25003)/50007=1193 -> vfp 217; fps_milli=49702 (PAL Genesis) -> vtotal=1200 -> vfp 224; fps_milli=59923 (NTSC Genesis) -> vtotal=996 -> vfp 20; fps_milli=60099 (NTSC SNES) -> vtotal=993 -> vfp 17. All fit inside the raised 256 clamp.

Note: the two harness slices below (B and C) each define their OWN local `*_vfp_for_fps` static that inlines the same rounded formula against the literal 59684000, and each calls `ayaneo_dsi_set_vfp` directly rather than this new `ayaneo_dsi_set_fps_milli`. That is the shape the harness slices were written against and it keeps each slice self-contained. `ayaneo_dsi_set_fps_milli` is still added here as the canonical single-choke-point helper and for any future core that wants the DSI-owned derivation; it does no harm and is the recommended target if a later refactor collapses the two per-core copies. See open item in section 7.

**A4. `/work/svoboda_lk/platform/mt6785/ddp_dsi.c` - optional, switch the readback divisor to the macro (line 1125).**

Current line 1125:
```c
	return 59684000u / vtotal;   /* line_rate(milliHz*lines) / vtotal = refresh in milliHz */
```
Optional change (recommended for read/write consistency on the shipped build):
```c
	return AYANEO_DSI_LINE_CONST / vtotal;   /* line_rate(milliHz*lines) / vtotal = refresh in milliHz */
```
This is precision-only for the GM_REFRESH readout. The readout truncates to tenths of a Hz (genesis_sd_run.c:254-255), and the 59667000 vs 59684000 divisor shifts the value by ~0.017 Hz, which does not change any displayed tenth. Safe either way; take it for consistency.

### Group B: Genesis ABI fps export + launch/region re-apply

This is one atomic bump. Author B1 through B7 together, build together, flash together.

**B1. `/work/svoboda_lk/emu/genesis/genesis_core_abi.h` - bump the ABI version (line 18).**

Current:
```c
#define GENESIS_CORE_ABI_VERSION 2u   /* v2: + set_ra_fast, sound_rebase (run-ahead/rewind audio continuity) */
```
Change to:
```c
#define GENESIS_CORE_ABI_VERSION 3u   /* v3: + fps_milli (core frame rate for dynamic per-core panel refresh; v2 was set_ra_fast, sound_rebase) */
```

**B2. `/work/svoboda_lk/emu/genesis/genesis_core_abi.h` - append the fps export as the LAST struct member (after `sound_rebase`, currently ends line 106, struct closes line 107).**

Current tail:
```c
	void  (*sound_rebase)(void);
};
```
Change to:
```c
	void  (*sound_rebase)(void);

	/* current emulated frame rate in milli-Hz (e.g. 59923 = 59.923 Hz NTSC, 49701 = 49.701 Hz PAL),
	 * read live from retro av_info. GPGX recomputes this on a region change (get_region updates
	 * system_clock + vdp_pal + lines_per_frame; fps = system_clock/lines_per_frame/MCYCLES_PER_LINE),
	 * but NEVER fires SET_SYSTEM_AV_INFO, so LK must POLL this after the region option takes effect
	 * and retune the panel vfp. */
	unsigned (*fps_milli)(void);
};
```
Append-only keeps every existing slot at its current offset. Do NOT widen av_info.

**B3. `/work/svoboda_lk/emu/genesis/genesis_core_exports.c` - implement `genesis_fps_milli`, placed directly after `genesis_aspect_x1000` (lines 250-258), before `genesis_sram_ptr` (line 260).**

After the existing `genesis_aspect_x1000` function, add:
```c
/* emulated frame rate in milli-Hz, read live (retro_get_system_av_info recomputes timing.fps from
 * system_clock/lines_per_frame/MCYCLES_PER_LINE each call, so this reflects a region change on the
 * very next read after check_variables has run inside retro_run). */
static unsigned genesis_fps_milli(void)
{
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	return (unsigned)(av.timing.fps * 1000.0 + 0.5);
}
```
av.timing.fps is the double set at libretro.c:3167. NTSC 53693175/262/3420 = 59.9227 -> 59923; PAL 53203424/313/3420 = 49.7015 -> 49701.

**B4. `/work/svoboda_lk/emu/genesis/genesis_core_exports.c` - append the pointer LAST in the positional `g_exports` initializer (currently ends `genesis_sound_rebase,` at line 293, closes line 294).**

Current:
```c
	genesis_set_av_skip,
	genesis_set_ra_fast,
	genesis_sound_rebase,
};
```
Change to:
```c
	genesis_set_av_skip,
	genesis_set_ra_fast,
	genesis_sound_rebase,
	genesis_fps_milli,
};
```
The initializer is positional (no designated initializers). `genesis_fps_milli` MUST be last, matching fps_milli being the last struct member.

**B5. `/work/svoboda_lk/emu/genesis/genesis_core_blob.ld` - LOAD-BEARING version literal (line 29).**

Current:
```
		LONG(2);                                 /* [1] version - MUST match GENESIS_CORE_ABI_VERSION */
```
Change to:
```
		LONG(3);                                 /* [1] version - MUST match GENESIS_CORE_ABI_VERSION */
```
The loader reads this as hdr[1] and sets `g_gen_dbg_loaderr=2` if it disagrees with GENESIS_CORE_ABI_VERSION (genesis_core_loader.c:65); the live struct.version mismatch path is loaderr=5 (loader.c:75). A mismatch boots straight back to the menu. The .ld cannot #include the header, so this literal is hand-kept and must move in lockstep with B1.

**B6. `/work/svoboda_lk/emu/genesis/genesis_core_blob.ld` - doc comment (line 12, not compiled).**

Current:
```
 *   u32 version = 2   (MUST match GENESIS_CORE_ABI_VERSION in genesis_core_abi.h - the loader rejects a
```
Change `= 2` to `= 3`.

**Genesis version-literal sites (all three):** genesis_core_abi.h:18 (B1), genesis_core_blob.ld:29 (B5, load-bearing), genesis_core_blob.ld:12 (B6, doc). genesis_core_exports.c:272 and genesis_core_loader.c:65/75 use the macro and auto-track. GENESIS_PERF_CAMPAIGN.md:180 is process doc, not a compiled literal.

**B7. `/work/svoboda_lk/emu/genesis/genesis_sd_run.c` - harness edits (loader/LK side).**

B7a. Extern block + GEN_VFP defines + new `genesis_vfp_for_fps` helper (lines 225-234). Current:
```c
extern unsigned int ayaneo_dsi_refresh_milli(void);   /* panel refresh in milli-Hz (ties into LCM work) */
extern void ayaneo_dsi_set_vfp(unsigned int vfp);     /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);
/* Panel vertical-front-porch per refresh rate (vtotal = 976 + vfp; refresh ~= 59.684 kHz / vtotal).
 * Genesis NTSC is 59.92 Hz, so vfp 20 -> vtotal 996 -> 59.923 Hz matches the core's native rate (the
 * vsync-locked present then paces emulation to it = no judder), like SNES uses vfp 17 for 60.11 Hz.
 * DEFAULT_VFP 23 (59.749 Hz) is restored for the menu / other cores on exit. */
#define GEN_VFP        20u
#define GEN_DEFAULT_VFP 23u
volatile unsigned g_gen_dbg_vfp;   /* live DSI vfp during the session (validates the switch) */
```
Change to:
```c
extern unsigned int ayaneo_dsi_refresh_milli(void);   /* panel refresh in milli-Hz (ties into LCM work) */
extern void ayaneo_dsi_set_vfp(unsigned int vfp);     /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);
extern unsigned int ayaneo_dsi_set_fps_milli(unsigned int fps_milli);  /* set panel refresh to target fps (milli-Hz), returns vfp (ddp_dsi.c) */
/* Panel vertical-front-porch per refresh rate (vtotal = 976 + vfp; refresh ~= 59.667 kHz / vtotal on
 * the shipped AYANEO_GBA build). Genesis NTSC is 59.92 Hz -> vfp 20 -> vtotal 996 matches the core's
 * native rate (the vsync-locked present then paces emulation to it = no judder), like SNES uses vfp 17.
 * PAL is ~49.70 Hz -> vfp 224 -> vtotal 1200 (needs ayaneo_dsi_set_vfp's clamp raised past 200 - see
 * the DSI mechanism change). DEFAULT_VFP 23 (59.749 Hz) is restored for the menu / other cores on exit. */
#define GEN_VFP        20u    /* NTSC fallback if the core reports no fps */
#define GEN_DEFAULT_VFP 23u
volatile unsigned g_gen_dbg_vfp;   /* live DSI vfp during the session (validates the switch) */

/* Map a core frame rate (milli-Hz) to the panel vfp that makes the vsync-locked present pace exactly
 * that rate. vtotal = 976 + vfp and refresh_milli = 59667000 / vtotal (shipped-build line const,
 * matches ayaneo_dsi_refresh_milli), so vfp = 59667000/fps_milli - 976, rounded. Guards a zero/insane
 * fps by falling back to the NTSC default. The DSI helper clamps the final vfp to its safe porch range. */
static unsigned genesis_vfp_for_fps(unsigned fps_milli)
{
	unsigned vtotal;
	if (fps_milli < 40000u || fps_milli > 65000u) return GEN_VFP;   /* implausible -> NTSC */
	vtotal = (59667000u + fps_milli / 2u) / fps_milli;             /* rounded 59667000/fps_milli */
	if (vtotal <= 976u) return 4u;                                 /* faster than the porch floor */
	return vtotal - 976u;
}
```
Correction folded in: use the shipped line constant 59667000 here (not 59684000) so the helper and ayaneo_dsi_refresh_milli agree on the shipped build and PAL Genesis lands on vfp 224. NTSC 59923 -> vtotal 996 -> vfp 20 (byte-identical to the old GEN_VFP, so NTSC is unchanged). PAL 49702 -> vtotal 1200 -> vfp 224. The [40000,65000] guard rejects a garbage av_info read. This DEPENDS on the raised clamp (A2); without it PAL pins to 200.

B7b. New module-scope re-tune flag, after the `s_gen_region` declaration (line 222). Current:
```c
static int s_gen_region;   /* 0 Auto, 1 USA (ntsc-u), 2 Europe (pal), 3 Japan (ntsc-j) */
```
Change to:
```c
static int s_gen_region;   /* 0 Auto, 1 USA (ntsc-u), 2 Europe (pal), 3 Japan (ntsc-j) */
static volatile int s_gen_refresh_retune;   /* >0: re-read core fps + retune panel vfp after the next committed frame(s) (real-time region switch) */
```

B7c. Launch-time vfp apply, forced-region aware (lines 448-452). Current:
```c
	/* Switch the panel to the Genesis NTSC rate (~59.92 Hz) so the vsync-locked present paces emulation
	 * to the core's native framerate (no periodic judder), like SNES runs the panel at 60.11 Hz.
	 * Restored to the stock 59.749 Hz for the menu on exit. */
	ayaneo_dsi_set_vfp(GEN_VFP);
	g_gen_dbg_vfp = ayaneo_dsi_get_vfp();
```
Change to:
```c
	/* Switch the panel to the CORE's native rate (NTSC ~59.92 Hz, PAL ~49.70 Hz) so the vsync-locked
	 * present paces emulation to it (no periodic judder / no speed error). Read fps live from the core:
	 * the region option pushed above has NOT been applied yet (check_variables runs inside retro_run,
	 * and no frame has run), so run one discarded frame first if a region was forced, then read.
	 * Restored to the stock 59.749 Hz for the menu on exit. */
	if (s_gen_region && c->set_option) {
		struct genesis_frame pf0; pf0.video = 0; pf0.width = 0; pf0.height = 0;
		c->run(&pf0);   /* let check_variables apply the forced region so fps_milli reflects it */
	}
	{
		unsigned fps = (c->fps_milli) ? c->fps_milli() : 0u;
		ayaneo_dsi_set_vfp(fps ? genesis_vfp_for_fps(fps) : GEN_VFP);
	}
	g_gen_dbg_vfp = ayaneo_dsi_get_vfp();
```
The forced region was pushed via set_option at lines 445-446, but check_variables only runs inside retro_run, so one c->run() must precede the fps read when a region is forced. The extra discarded frame is harmless: the punch-hole block at lines 459-479 already runs up to 20 warm-up frames before presenting. `pf0.width/height` are zeroed for defensive parity with the punch-hole block style. `c->fps_milli` is null-guarded. NTSC path yields vfp 20, identical to today.

B7d. GM_REGION handler (lines 275-279) - THE real-time fix. Current:
```c
	case GM_REGION: if (dir) { s_gen_region = (s_gen_region + dir + 4) % 4;
		if (s_menu_c && s_menu_c->set_option)
			s_menu_c->set_option("genesis_plus_gx_region_detect", gen_region_opt(s_gen_region));
		ayaneo_set_gen_region(s_gen_region); genesis_settings_touch();
		mput(s_mstat, "Region set (reset for full effect)"); } break;
```
Change to:
```c
	case GM_REGION: if (dir) { s_gen_region = (s_gen_region + dir + 4) % 4;
		if (s_menu_c && s_menu_c->set_option)
			s_menu_c->set_option("genesis_plus_gx_region_detect", gen_region_opt(s_gen_region));
		ayaneo_set_gen_region(s_gen_region); genesis_settings_touch();
		/* Real-time refresh switch: the core option is now queued (s_opt_dirty), but check_variables -
		 * which recomputes system_clock/vdp_pal/lines_per_frame, hence fps - only runs inside the next
		 * retro_run(). The game keeps running underneath the Pico menu, so the very next c->run() in the
		 * session loop applies the region and updates fps. Arm a deferred re-tune that the session loop
		 * fires after that frame; do NOT poll fps here (it would still read the OLD rate). */
		s_gen_refresh_retune = 2;   /* re-read fps + retune vfp for the next 2 committed frames */
		mput(s_mstat, "Region set (some games need Reset)"); } break;
```
Do NOT poll c->fps_milli() synchronously here: set_option only sets s_opt_dirty, and check_variables (libretro.c:3888, gated on GET_VARIABLE_UPDATE) runs inside the next retro_run. A same-handler poll returns the OLD fps.

B7e. Session-loop consumer, immediately after the committed frame bookkeeping (after line 568, `g_gen_dbg_frames++;`). Current:
```c
		em0 = gpt4_get_current_tick();
		c->run(&fr);                                 /* committed frame */
		fe = (gpt4_get_current_tick() - em0) / 13u;  /* per-frame emu cost (us) for the FF cap */
		g_gen_dbg_frames++;
```
Change to (append after `g_gen_dbg_frames++;`):
```c
		em0 = gpt4_get_current_tick();
		c->run(&fr);                                 /* committed frame */
		fe = (gpt4_get_current_tick() - em0) / 13u;  /* per-frame emu cost (us) for the FF cap */
		g_gen_dbg_frames++;
		/* Real-time panel refresh follow: a GM_REGION change armed s_gen_refresh_retune; the region
		 * option was applied by the c->run() just above (check_variables -> get_region recomputes fps),
		 * so read the (now new) fps and retune the panel vfp. Retry a couple of frames in case the
		 * variable-update flag was consumed a frame late. No extra wait_for_vsync here (double-wait = 30fps). */
		if (s_gen_refresh_retune > 0 && c->fps_milli) {
			unsigned fps = c->fps_milli();
			if (fps) { ayaneo_dsi_set_vfp(genesis_vfp_for_fps(fps)); g_gen_dbg_vfp = ayaneo_dsi_get_vfp(); }
			s_gen_refresh_retune--;
		}
```
Placed on the COMMITTED-frame path only (not FF/rewind), so a region change during FF or rewind still lands on the next normal frame. Adds NO wait_for_vsync.

B7f. GM_REFRESH readout (lines 254-255) - NO CHANGE. It reads live DSI_VFP_NL via ayaneo_dsi_refresh_milli and returns 59667000/(976+vfp) (after A4) or 59684000/... (without A4). Since genesis_vfp_for_fps is the exact inverse of that formula/const, after a PAL switch vfp 224 -> refresh reads ~49722 mHz -> "49.7 Hz"; after NTSC vfp 20 -> "59.9 Hz". The readout tracks the switch automatically. Noted so nobody "fixes" it.

Sites NOT touched (confirmed correct): genesis exit restore GEN_DEFAULT_VFP at line 704; genesis_bench_body headless path at line 729 (sets no vfp, needs none); host_test.c (does not include genesis_core_abi.h, calls retro_* directly).

### Group C: snes9x ABI fps export + launch apply

One atomic bump. Version namespace is independent of Genesis (SNS1 vs SEG1); the snes version need not match the Genesis version, only its own .ld literal.

**C1. `/work/svoboda_lk/emu/snes9x/snes_core_abi.h` - bump the version (line 14).**

Current:
```c
#define SNES_CORE_ABI_VERSION 1u
```
Change to:
```c
#define SNES_CORE_ABI_VERSION 2u
```

**C2. `/work/svoboda_lk/emu/snes9x/snes_core_abi.h` - append fps export LAST (after `state_load_ra`, line 97, struct closes line 98). Do NOT widen av_info (lines 46-48).**

Append as the last struct member:
```c
	/* emulated native refresh in milliHz (e.g. 60099 = 60.0988 Hz NTSC, 50007 = 50.0070 Hz PAL),
	 * from retro av.timing.fps * 1000. Region is decided at load (ROM auto-detect / snes9x_region),
	 * so this is valid post-load and re-poll-able. LK derives the panel vfp from it. */
	unsigned (*fps_milli)(void);
```
Keep av_info at its 5-arg signature (base_w, base_h, max_w, max_h, sample_rate) byte-for-byte.

**C3. `/work/svoboda_lk/emu/snes9x/snes_core_exports.cpp` - implement `snes_fps_milli`, inserted just above `snes_aspect_x1000` (which is lines 260-267; snes_av_info ends line 258).**

```cpp
static unsigned snes_fps_milli(void)
{
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	/* av.timing.fps is set in libretro.cpp retro_get_system_av_info (line 1153):
	 * NTSC 21477272/357366 = 60.0988, PAL 21281370/425568 = 50.0070. Region comes from
	 * Settings.PAL via retro_get_region (line 2319), decided at retro_load_game. */
	return (unsigned)(av.timing.fps * 1000.0 + 0.5);
}
```

**C4. `/work/svoboda_lk/emu/snes9x/snes_core_exports.cpp` - append the pointer LAST in `g_exports` (tail at lines 313-315).**

Current:
```cpp
	snes_state_save_ra,
	snes_state_load_ra,
};
```
Change to:
```cpp
	snes_state_save_ra,
	snes_state_load_ra,
	snes_fps_milli,
};
```

**C5. `/work/svoboda_lk/emu/snes9x/snes_core_blob.ld` - LOAD-BEARING version literal (line 32).**

Current:
```
		LONG(1);                              /* [1] version */
```
Change to:
```
		LONG(2);                              /* [1] version - MUST match SNES_CORE_ABI_VERSION */
```
snes_core_loader.c:64 checks `hdr[1] != SNES_CORE_ABI_VERSION` (loaderr=2), line 74 checks the live struct. Also update the doc comment near line 17 (`u32 version = 1` -> `2`, not compiled).

**snes version-literal sites:** snes_core_abi.h:14 (C1), snes_core_blob.ld:32 (C5, load-bearing), snes_core_blob.ld ~17 (doc). snes_core_loader.c:64/74 use the macro and auto-track. No separate loader typedef reads slots by offset.

**C6. `/work/svoboda_lk/emu/snes9x/snes_sd_run.c` - extern + defines + `snes_vfp_for_fps` (lines 109-113, and externs at 32-33).**

First, extend the extern block (lines 32-33). Current:
```c
extern void     ayaneo_dsi_set_vfp(unsigned int vfp);   /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);           /* read-back to validate the switch */
```
Change to:
```c
extern void     ayaneo_dsi_set_vfp(unsigned int vfp);   /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);           /* read-back to validate the switch */
extern unsigned int ayaneo_dsi_refresh_milli(void);     /* live panel refresh in milli-Hz (ddp_dsi.c) - for the FF budget */
extern unsigned int ayaneo_dsi_set_fps_milli(unsigned int fps_milli); /* set panel refresh to target fps (milli-Hz), returns vfp (ddp_dsi.c) */
```
The `ayaneo_dsi_refresh_milli` extern is required by the FF-budget change (D3); this file does not declare it today. `ayaneo_dsi_set_fps_milli` is declared for parity even though C7 calls set_vfp directly.

Then the vfp defines (lines 109-113). Current:
```c
/* Panel vertical-front-porch per refresh rate. Stock vfp 23 -> vtotal 999 -> 59.749 Hz
 * (GB/GBC/GBA/menu). SNES uses vfp 17 -> vtotal 993 -> ~60.11 Hz (0.02% off its native
 * 60.0988 Hz) so the vsync-locked present in ayaneo_snes_show_frame is smooth. */
#define SNES_VFP      17u
#define DEFAULT_VFP   23u
```
Change to:
```c
/* Panel vfp derived from the core's native refresh. vtotal = 976 + vfp; shipped-build line const
 * 59667000, so refresh_milliHz = 59667000 / vtotal, i.e. vfp = round(59667000 / fps_milli) - 976.
 * NTSC 60099 -> vfp 17 (matches the old fixed value); PAL 50007 -> vfp 217. The 217 needs the
 * ddp_dsi clamp lifted from 200 to >=225; without it vfp caps at 200 = 50.75 Hz and PAL runs ~1.5% fast. */
#define SNES_LINE_CONST 59667000u
#define SNES_VFP_NTSC   17u          /* fallback if fps_milli export missing */
#define DEFAULT_VFP     23u
static unsigned snes_vfp_for_fps(unsigned fps_milli)
{
	unsigned vt, vfp;
	if (fps_milli < 20000u || fps_milli > 130000u) return SNES_VFP_NTSC; /* sanity: 20..130 Hz */
	vt = (SNES_LINE_CONST + fps_milli / 2u) / fps_milli;   /* round(const/fps) */
	vfp = (vt > 976u) ? (vt - 976u) : 4u;
	return vfp;
}
```
Correction folded in: use the shipped 59667000, so PAL 50007 -> vtotal 1193 -> vfp 217 (not 218), matching the Genesis slice and the DSI mechanism on the shipped build. NTSC 60099 -> vfp 17, identical to the old fixed value. Do NOT add a local min(vfp,200) here; the clamp is the single choke point in ayaneo_dsi_set_vfp.

**C7. `/work/svoboda_lk/emu/snes9x/snes_sd_run.c` - launch vfp apply, region-aware (lines 577-581).**

Current:
```c
	/* Switch the panel to ~60.11 Hz for SNES (vfp swap). The vsync-locked present in
	 * ayaneo_snes_show_frame then paces emulation to the panel scan - smooth, tear-free,
	 * no 13 MHz busy-wait needed. Restored to 59.749 Hz on exit below. */
	ayaneo_dsi_set_vfp(SNES_VFP);
	g_snes_dbg_vfp = ayaneo_dsi_get_vfp();   /* read-back: should equal SNES_VFP (17) */
```
Change to:
```c
	/* Switch the panel to the CORE's native refresh (region-aware): NTSC ROMs -> ~60.09 Hz (vfp 17),
	 * PAL ROMs -> ~50 Hz (vfp 217). Region is auto-detected at c->load(); poll fps now. The vsync-locked
	 * present in ayaneo_snes_show_frame then paces emulation to the panel scan. Restored to 59.749 Hz on
	 * exit below. NOTE: PAL vfp 217 needs the ddp_dsi [4,200] clamp raised to >=225; until that ships PAL
	 * is capped at 200 (50.75 Hz, ~1.5% fast). */
	{
		unsigned fps_milli = (c->fps_milli) ? c->fps_milli() : 60099u;
		ayaneo_dsi_set_vfp(snes_vfp_for_fps(fps_milli));
	}
	g_snes_dbg_vfp = ayaneo_dsi_get_vfp();   /* read-back: derived vfp (17 NTSC / 217 PAL) */
```
c is validated non-null at line 515, c->load succeeded at line 527, av_info ran at line 529, and Settings.PAL is set during Memory.LoadROMMem (memmap.cpp:2370-2376), so the region is settled here. No pre-run frame is needed for snes (unlike Genesis) because there is no forced-region set_option at launch. The null-guard protects a mixed build.

**C8. snes9x has NO live in-session region toggle - NO CHANGE for a region re-apply.** The Pico menu enum (lines 334-336) has no SM_REGION; the option table s_opt_def (lines 356-361) covers only OI_ASPECT/OI_OVERSCAN/OI_AUDIO/OI_HIRES; the "NTSC"/"PAL" strings in s_asp_ch (line 348) set the display key snes9x_aspect (aspect ratio only, get_aspect_ratio libretro.cpp ~1105-1125), NOT Settings.PAL and NOT retro_get_region. There is no set_option("snes9x_region",...) anywhere in snes_sd_run.c. Region is fixed at retro_load_game, so the launch apply in C7 FULLY covers PAL ROMs. snes9x therefore needs strictly fewer edits than Genesis and has no session-loop retune. Do NOT mistake the aspect "PAL"/"NTSC" entries for a region toggle.

Sites NOT touched: snes exit restore DEFAULT_VFP at line 1163; the FF-budget line 850 is handled in Group D.

### Group D: Audio evidence + FF/run-ahead budget scaling (LK side, no ABI/version/.ld impact)

**D1. `/work/svoboda_lk/platform/mt6785/ayaneo_audio.c` - NO CHANGE (audio evidence).** The Genesis/SNES path self-adapts: `ayaneo_snes_audio_submit` derives its resample step only from `src_hz` (the core's av_info sample_rate) and the fixed ring const GBC_DST_HZ=48000 (line 1030); the step math (lines 1432-1433) has no panel-fps, vfp, or refresh term. `src_hz` is region-independent (SNES DSP ~32040 via S9xGetAudioSampleRate, libretro.cpp:1150-1152; Genesis fixed 44100 via genesis_sd_run.c seed at line 382 / guard at line 416). The panel governs how often submit() is called and how many frames arrive per present; the resampler locks to whatever rate the panel paces, so a 50 Hz panel simply fills the ring slower while the AFE drains it at 48 kHz. Correct pitch, no drift, at any panel fps including ~50 Hz. Do NOT re-tune the resampler for PAL: that would introduce a pitch bug where none exists.

**D2. `/work/svoboda_lk/platform/mt6785/ayaneo_audio.c` - GBA feed-forward path, NO CHANGE for this campaign.** `ayaneo_gba_audio_set_rate_milli` rejects hz1000<50000 at line 1177. GBA (gpSP) has no PAL region and no av_info/region path (fixed GBA_SRC_HZ=65536), so this helper is never called below 59.7275 Hz today. The inc clamp 44000..52000*GA_FRAC (lines 1180-1181, GA_FRAC=256 at line 1157) already spans the PAL output case because the box-resampler output rate stays ~48 kHz regardless of the 49.7 vs 60 Hz input cadence. FUTURE-PROOF only if this helper is ever generalized to a PAL-capable core: change the line-1177 floor to `if (hz1000 < 49000 || hz1000 > 70000)`; the 44000/52000 inc clamp needs no widening. Out of scope for this campaign.

**D3. `/work/svoboda_lk/emu/genesis/genesis_sd_run.c` - FF budget rescale (completeness), lines 584-591.**

Current (the ff>0 block):
```c
		if (ff > 0) {
			int raw = 2 + (ff * (10 - 2)) / 255;
			unsigned int blit = g_dbg_blit_us < 15500u ? g_dbg_blit_us : 0u;
			unsigned int fef = fe ? fe : 2500u;
			int cap = (int)((15500u - blit) / fef);
```
Change to derive the budget from the live panel refresh:
```c
		if (ff > 0) {
			int raw = 2 + (ff * (10 - 2)) / 255;
			unsigned int rmilli = ayaneo_dsi_refresh_milli();
			unsigned int budget_us = rmilli ? (1000000000u / rmilli) : 16666u;   /* 1e9/milliHz = 1e6/panel_fps */
			if (budget_us > 1200u) budget_us -= 1200u;   /* ~1.2ms present-margin the old 15500-vs-16666 baked in */
			unsigned int blit = g_dbg_blit_us < budget_us ? g_dbg_blit_us : 0u;
			unsigned int fef = fe ? fe : 2500u;
			int cap = (int)((budget_us - blit) / fef);
```
`ayaneo_dsi_refresh_milli` is already declared extern in this file (line 225). At 50 Hz the real window is ~20000us, so the old 15500 was conservative-but-safe, never wrong. budget_us tracks a real-time vfp switch with no new plumbing. The mult floor at line 590 is unchanged. Unit care: use 1000000000u/rmilli, NOT 1000000u/rmilli (off by 1000x); guard rmilli!=0.

**D4. `/work/svoboda_lk/emu/snes9x/snes_sd_run.c` - FF budget rescale (completeness), line 850.**

Current (block ~836-852):
```c
			unsigned int blit = g_snes_show_us;
			unsigned int avail = (15500u > blit + rend) ? (15500u - blit - rend) : 0u;
			int cap = 1 + (int)(avail / skip);
```
Change to:
```c
			unsigned int blit = g_snes_show_us;
			unsigned int rmilli = ayaneo_dsi_refresh_milli();
			unsigned int budget_us = rmilli ? (1000000000u / rmilli) : 16666u;
			if (budget_us > 1200u) budget_us -= 1200u;
			unsigned int avail = (budget_us > blit + rend) ? (budget_us - blit - rend) : 0u;
			int cap = 1 + (int)(avail / skip);
```
The required `extern unsigned int ayaneo_dsi_refresh_milli(void);` is added in C6 (this file does not declare it otherwise). The mult clamps at lines 854-855 are unchanged. `s_snes_hz1000` is not used here; this uses the register-direct refresh so it is correct from the first frame.

**D5. Run-ahead OPP tiers and the "16.7 ms budget" prose - NO CHANGE.** snes_sd_run.c s_snes_ra_opp (line 58) and genesis_sd_run.c s_gen_ra_opp (line 82) are static MHz ladders with no budget arithmetic; the "16.7 ms" text at snes:53-54 and genesis:79-81 is descriptive prose, not a live constant. The 60 Hz budget is the tighter constraint, so tiers sized for it stay valid at the looser 50 Hz. Optional prose reword ("~16.7 ms at 60 Hz, ~20 ms at 50 Hz PAL") only if desired; no functional change. The other 15500/155000/16.7ms hits (genesis:725 comment, genesis:823-826 bench metric) are headless-bench "implied rewind speed" numbers, not gameplay budgets; do not touch them.

## 3. Real-time region-switch flow (Genesis GM_REGION)

This is the headline. Step by step, all on the single genesis_emu session thread:

1. The game is running under the open Pico menu. The user moves the Region item to Europe. `gm_change` GM_REGION (B7d) runs: it advances `s_gen_region`, calls `s_menu_c->set_option("genesis_plus_gx_region_detect", "pal")` (which only sets the blob's `s_opt_dirty` flag), calls `ayaneo_set_gen_region` + `genesis_settings_touch`, sets `s_gen_refresh_retune = 2`, and shows "Region set (some games need Reset)". It does NOT poll fps and does NOT touch vfp here.
2. Control returns to the session loop. The game keeps running because rewind (line 504) and FF (line 583) are gated off while the Pico menu is open, so the committed `c->run(&fr)` at line 566 is the only c->run() per iteration.
3. Inside that c->run(), the blob answers GET_VARIABLE_UPDATE from `s_opt_dirty`, so check_variables (libretro.c:3888) fires and get_region recomputes `system_clock` (loadrom.c:1170), `vdp_pal` (loadrom.c:1167), and `lines_per_frame` (libretro.c:1627). audio_set_rate stays 44100 (region-independent). The core's fps is now the new PAL rate. This is why polling must be deferred: a poll inside the handler in step 1 would read the pre-change rate.
4. Right after that committed c->run() and its bookkeeping (`g_gen_dbg_frames++;`), the session-loop consumer (B7e) runs: `s_gen_refresh_retune > 0` and `c->fps_milli` is non-null, so it reads `fps = c->fps_milli()` (now 49701), computes `genesis_vfp_for_fps(49701) = 224`, calls `ayaneo_dsi_set_vfp(224)`, updates `g_gen_dbg_vfp`, and decrements the counter.
5. Because the panel is in continuous SYNC_PULSE VDO mode, the DSI engine re-reads DSI_VFP_NL at the next frame boundary and vtotal becomes 1200 with NO DSI reset. The vsync-locked present at primary_display.c:1081 (waits FRAME_DONE) now paces one emulated frame per ~49.72 Hz refresh, so speed and audio pitch are correct from that frame on. No extra wait_for_vsync is added anywhere (the double-wait = 30 fps bug).
6. The counter of 2 makes the consumer retry on the next committed frame as a belt-and-suspenders margin in case GET_VARIABLE_UPDATE was consumed one frame late by a throwaway FF/run-ahead c->run() (which does not decrement the counter but does consume s_opt_dirty; fps is global state, so the committed-frame read still sees the new value). GM_REFRESH (line 254) reflects the new rate automatically on the next menu paint because ayaneo_dsi_refresh_milli reads the live register.

Ordering rule restated: arm-then-consume, never poll-in-handler. The re-tune is consumed on the COMMITTED-frame path only, so a region change during FF or rewind still lands cleanly on the next normal frame. Some Genesis titles latch region at boot and only fully honour the change on a hard reset; the "some games need Reset" status keeps that honest, and GM_RESET (line 300) re-runs the launch path (which re-reads fps and re-applies vfp). Optionally arm `s_gen_refresh_retune = 2` in the GM_RESET handler too so a post-reset region also retunes; trivially in-scope, confirm if wanted.

snes9x has no equivalent flow: no live region toggle exists (C8), so its launch-time apply (C7) is the whole story for PAL there.

## 4. Audio + FF/budget notes

- Audio needs ZERO code. Genesis/SNES go through `ayaneo_snes_audio_submit`, whose resample step is a function of the core sample_rate (region-independent: SNES ~32040, Genesis 44100) and the fixed 48 kHz ring, with no panel-fps term. The panel paces call frequency; the resampler self-locks. This holds at 50 Hz. Do not re-tune it (D1).
- GBA feed-forward (`ayaneo_gba_audio_set_rate_milli`, floor <50000 at line 1177) is untouched: GBA has no PAL and never calls it below 59.7 Hz. Only relevant if that helper is ever generalized (change the floor to 49000; the inc clamp already covers PAL output). Not this campaign (D2).
- Optional audio re-seed after a mid-session vfp change: `ayaneo_audio_reverse_flip` (lines 1410-1421) already re-seats the ring write cursor to a clean ~85 ms lead and resets all three resampler carries without wiping the ring. The Genesis GM_REGION session consumer MAY call it right after `ayaneo_dsi_set_vfp` for faster reconvergence; it is a nicety, not required for correctness. GBA generalized path would instead call `ayaneo_gba_audio_set_rate_milli(new_milliHz)`, not reverse_flip.
- FF/run-ahead budget (D3/D4) is a completeness item, not a correctness fix. The hardcoded 15500us assumes one 60 Hz frame; at 50 Hz the real window is ~20000us, so the old value merely under-caps FF (fewer FF frames per present), never runs the game wrong. Rescaling to 1000000000/refresh_milli - 1200 makes FF track the live refresh. Land it after the display switch so it is actually exercised at PAL. The mult floors (genesis 2, snes 2) guarantee an over-tight budget can never stall FF.
- Run-ahead OPP tiers reference "16.7 ms" only in prose; no scaling needed (D5).

## 5. Build / sign / flash

The whole thing is one blob+lk change per core touched. Never mix an old blob with a fresh lk_a, and never mix a fresh blob with an old lk_a: the ABI version literal is baked into both, and a mismatch is loaderr=2 (boot back to menu).

1. Build everything in one run:
   ```
   ./build_ayaneo_gba_sd.sh
   ```
   (or `./build_ayaneo_gba_sd.sh clean` for from-scratch). This rebuilds all four core blobs (gpSP, GBC, snes9x, Genesis; lines 30-45), then relinks lk_a with `make k85v1_64 AYANEO_GBA_SD=yes -jN` (line 49, which compiles genesis_core_loader.c / snes_core_loader.c against the new ABI headers), then signs (line 62), then repacks boot_b (lines 87-88).
2. Pre-flash version guard (catch a forgotten .ld literal before trusting boot_b):
   ```
   od -An -tx4 -j4 -N4 /work/svoboda_lk/emu/genesis/core_genesis.blob
   ```
   Confirm the version word equals GENESIS_CORE_ABI_VERSION (3). Repeat for the snes blob against 2. If they diverge, the device would hit loaderr=2 and the core silently falls back to the menu.
3. Copy artifacts to the Windows-visible drop:
   ```
   cp /work/svoboda_lk/out/lk_a_gba_sd_signed.img /mnt/c/pairmini/
   ```
   and copy the packed boot_b image the script produced into /mnt/c/pairmini as well.
4. Signing: only lk_a is signed (RSA-PSS, `rsa_pss_saltlen:-1` i.e. saltlen = SHA-256 digest length = 32, key tools/ayaneo/keys/img_prvk.pem, partition guard `names == ['lk','lk_main_dtb']`, PART_SIZE 2 MB). boot_b is never signed (raw data read via partition_read). The core-blob ABI bump changes NO signing behaviour; the standard lk_a re-sign the master script already runs covers it.
5. Flash lk_a and boot_b TOGETHER, one fastboot command per shell call, serial 0123456789ABCDEF, partition name lk_a. IMPORTANT: always reflash boot_b on any core-blob change; the script's "boot_b unchanged, skip" optimization keys only on the SNES asset-pack SHA (lines 93-98), not on the core blob bytes, so it may wrongly say skip after an ABI bump. Ignore that hint. Each flash is its own call:
   ```
   fastboot -s 0123456789ABCDEF flash lk_a /mnt/c/pairmini/lk_a_gba_sd_signed.img
   ```
   then (separate call) flash boot_b with the packed boot_b image, then (separate call):
   ```
   fastboot -s 0123456789ABCDEF reboot bootloader
   ```
   Never chain two fastboot verbs in one invocation (the stack crashes on concurrency). Never run `oem sd-probe` (it does not exist in this tree; the real probes are `oem gen-probe` / `oem snes-probe`). After a reflash or reboot, poll `fastboot devices` until 0123456789ABCDEF appears with a real transport (not "???") before the next single command; allow ~24s post-reflash and retry on a dropped link.

## 6. On-device validation checklist

Genesis is the primary PAL vehicle (it has GM_REGION); snes9x PAL cannot be exercised until a snes region menu exists (out of scope here), so snes validation is NTSC-only. During a live Genesis session there is no existing USB readout of panel Hz (genesis_sd_run.c never writes g_dbg_hz1000; oem diag's hz1000 is the menu/GBA-loop value). Recommended small independent add for the live-toggle proof: fold `refresh_mHz` + `vfp` (register-direct via ayaneo_dsi_refresh_milli / ayaneo_dsi_get_vfp) into the existing `oem diag` output as an extra INFO line, unconditionally, so it works for every running core; this bumps no ABI. Alternatively register a dedicated `oem panelhz`.

(a) NTSC sanity. Launch a Genesis NTSC ROM. Expect the panel at ~59.9 Hz. Read vfp via oem diag (or the on-screen GM_REFRESH item): vfp = 20, GM_REFRESH shows "59.9 Hz". Launch an NTSC snes9x ROM: `fastboot -s 0123456789ABCDEF oem diag` reports snes hz1000 ~60088 and snes_vfp=17 (g_snes_dbg_hz1000 is the live in-session average, aliased by the `#define s_snes_hz1000 g_snes_dbg_hz1000` at snes_sd_run.c:93-94). PASS = NTSC vfp/refresh unchanged from today.

(b) PAL mechanism (launch). Launch a Genesis game with the persisted region set to Europe. Expect vfp 224, GM_REFRESH "49.7 Hz", panel refresh ~49.72 Hz, game running at correct speed and pitch. If you instead read ~50.75 Hz (vfp pinned to 200), the clamp raise (A2) did not land; that is a missing-fix, not a partial PASS.

(c) Real-time toggle (headline). With a Genesis game running, open the Pico menu and toggle Region from USA to Europe. Poll panel refresh via the oem diag INFO line (or panelhz) immediately before and after the toggle: it must move from ~59.9 Hz (vfp 20) to ~49.7 Hz (vfp 224) within a frame or two of the toggle, and the game speed/pitch must correct at that moment. Toggle back to USA and confirm it returns to ~59.9 Hz. Pick a test ROM whose region GPGX honours live (or reset as part of the step) since some titles only fully apply region on reset.

(d) Banding / flicker at high vtotal. Visually inspect the PAL Genesis screen at vtotal 1200 (vfp 224) for flicker, horizontal banding, or brightness droop. This is A55-specific scanout and CANNOT be host-tested. If it flickers, the fallback is the st7703 charge-pump register ST7703_CPUMP (0x17, currently 0x48) in the panel init table (dev/lcm/st7703_hd720_dsi_vdo/...): adjust ONE reg byte, re-sign lk_a, reflash, re-check. If a working screenshot path exists (tools/ayaneo/gba/fastboot_menu_shot.py implies one), capture before/after; otherwise judge by eye on device.

(e) Audio. Confirm PAL Genesis audio pitch is correct (not sped-up) at the corrected refresh, and that toggling region does not click/drop beyond a brief reconverge. If reconvergence is slow and you took the optional reverse_flip call, confirm it helps.

(f) FF under PAL (non-blocking). Confirm fast-forward still engages during a PAL Genesis session and audio pitches up; it need not hit the theoretical max multiplier for a PASS.

## 7. Risks + rollback

- Biggest risk: st7703 electrical stability at vtotal ~1200 (Genesis PAL, shipped build) or ~1201 (non-shipped). Untestable on host. Mitigation: 256 clamp keeps the ceiling modest; ST7703_CPUMP is the documented tuning knob; capping PAL at a slightly higher refresh is a last resort. Rollback for flicker only: revert the CPUMP byte or, if severe, revert A2 to `> 200u` (PAL then pins to 50.75 Hz but the panel stays in the tested envelope).
- ABI/version lockstep (regressed before): if the .ld literal (B5 / C5) is not moved with the header macro, the blob loads then loaderr=2 and the core boots back to the menu. Mitigation: the pre-flash od guard (step 2), and always run the full build script so blob and lk_a come from one tree. Rollback: revert the version bump AND the .ld literal AND the appended slot together (all three of B1/B2/B5 or C1/C2/C5), then rebuild; NTSC behaviour returns to today.
- Line-constant / one-line PAL ambiguity: the plan pins the shipped constant 59667000 across the DSI helper, Genesis, and snes9x, so all three agree on PAL vfp 217 (SNES) / 224 (Genesis). If the build ever undefines AYANEO_GBA, the DSI macro auto-switches to 59684000 (218/225) but the two harness helpers hardcode 59667000 and would then be one line off from the readback. That is a ~0.03 Hz reporting inconsistency, not a functional break, and the shipped build is not affected. Open item: hoist the derivation into the single DSI helper `ayaneo_dsi_set_fps_milli` (A3) and have both harnesses call it instead of their local copies, so the constant lives in exactly one place; recommended as a follow-up, not required for this campaign.
- Double-wait regression: any accidental wait_for_vsync on the switch path drops to 30 fps. The plan adds none; the retune is a bare register write that lands at the next VDO frame boundary. Guard against reviewers "helpfully" adding a wait.
- No wait/no-poll ordering bug: polling fps synchronously in GM_REGION reads the OLD rate. The deferred arm-then-consume is the only correct shape; do not collapse it into the handler.

## 8. Ordered task checklist

1. A1 ddp_dsi.c: add `AYANEO_DSI_LINE_CONST` macro (gated on AYANEO_GBA, 59667000 else 59684000) after line 55.
2. A2 ddp_dsi.c: raise the vfp upper clamp 200u -> 256u (line 1104 only).
3. A3 ddp_dsi.c: add `ayaneo_dsi_set_fps_milli` after line 1126 (calls ayaneo_dsi_set_vfp, no re-poke, no wait_for_vsync).
4. A4 ddp_dsi.c (optional): switch line 1125 divisor to `AYANEO_DSI_LINE_CONST`.
5. B1 genesis_core_abi.h: version 2u -> 3u (line 18).
6. B2 genesis_core_abi.h: append `unsigned (*fps_milli)(void);` as the LAST struct member.
7. B3 genesis_core_exports.c: add `genesis_fps_milli` after genesis_aspect_x1000.
8. B4 genesis_core_exports.c: append `genesis_fps_milli,` LAST in g_exports.
9. B5 genesis_core_blob.ld: LONG(2) -> LONG(3) (line 29, load-bearing).
10. B6 genesis_core_blob.ld: doc comment `= 2` -> `= 3` (line 12).
11. B7a genesis_sd_run.c: add the `ayaneo_dsi_set_fps_milli` extern and the `genesis_vfp_for_fps` helper (const 59667000); annotate GEN_VFP as NTSC fallback.
12. B7b genesis_sd_run.c: add `static volatile int s_gen_refresh_retune;` after s_gen_region.
13. B7c genesis_sd_run.c: launch apply (lines 448-452) -> run one discarded frame if a region was forced, then read c->fps_milli() and set vfp via genesis_vfp_for_fps.
14. B7d genesis_sd_run.c: GM_REGION handler (275-279) -> arm `s_gen_refresh_retune = 2`, update status text; do NOT poll fps here.
15. B7e genesis_sd_run.c: session-loop consumer after line 568 -> read fps, set vfp, decrement; no wait_for_vsync.
16. C1 snes_core_abi.h: version 1u -> 2u (line 14).
17. C2 snes_core_abi.h: append `unsigned (*fps_milli)(void);` LAST; keep av_info 5-arg.
18. C3 snes_core_exports.cpp: add `snes_fps_milli` above snes_aspect_x1000.
19. C4 snes_core_exports.cpp: append `snes_fps_milli,` LAST in g_exports.
20. C5 snes_core_blob.ld: LONG(1) -> LONG(2) (line 32, load-bearing) + doc comment.
21. C6 snes_sd_run.c: add externs `ayaneo_dsi_refresh_milli` and `ayaneo_dsi_set_fps_milli`; replace SNES_VFP with SNES_LINE_CONST=59667000 + SNES_VFP_NTSC + `snes_vfp_for_fps` (PAL -> vfp 217).
22. C7 snes_sd_run.c: launch apply (577-581) -> read c->fps_milli() (fallback 60099), set vfp via snes_vfp_for_fps.
23. C8 snes_sd_run.c: confirm NO region-toggle wiring is needed (no SM_REGION; aspect PAL/NTSC is display-only).
24. D3 genesis_sd_run.c: FF budget (584-591) -> derive budget_us from ayaneo_dsi_refresh_milli (1000000000/rmilli - 1200), replace both 15500u uses.
25. D4 snes_sd_run.c: FF budget (line 850) -> same rescale (extern added in C6).
26. D1/D2/D5: confirm audio path and run-ahead OPP tiers need no change (evidence only).
27. Build: `./build_ayaneo_gba_sd.sh`; then od-check both blob version words (genesis=3, snes=2).
28. Copy lk_a_gba_sd_signed.img and the packed boot_b to /mnt/c/pairmini.
29. Flash lk_a, then boot_b, then reboot bootloader (one fastboot command per call, serial 0123456789ABCDEF, boot_b MUST be reflashed).
30. Validate on device: NTSC sanity (a), PAL launch (b), real-time toggle (c), banding/flicker + CPUMP fallback (d), audio (e), FF under PAL (f).

## 9. Implementation status

Groups A through D are IMPLEMENTED in the working tree and the tree builds clean via `build_ayaneo_gba_sd.sh` (EXIT 0). Done:

- A1-A4 ddp_dsi.c: AYANEO_DSI_LINE_CONST macro (build-gated 59667000/59684000), vfp clamp 200 -> 256, ayaneo_dsi_set_fps_milli() helper, refresh_milli divisor switched to the macro.
- B1-B7 genesis: ABI 2 -> 3, fps_milli() appended last in the struct + exports table, genesis_core_blob.ld LONG(2) -> LONG(3) + doc, genesis_fps_milli impl, genesis_vfp_for_fps helper, s_gen_refresh_retune flag, region-aware launch apply (one discarded frame if a region is forced), GM_REGION arm, session-loop retune consumer.
- C1-C8 snes9x: ABI 1 -> 2, fps_milli() appended last, snes_core_blob.ld LONG(1) -> LONG(2) + doc, snes_fps_milli impl, snes_vfp_for_fps helper (PAL vfp 217), region-aware launch apply, refresh externs. Confirmed no live region toggle exists (launch-only).
- D3/D4 FF budget: derived from ayaneo_dsi_refresh_milli (1e9/rmilli - 1200) at both sites; the margin subtraction is folded into an initializer (declaration-first) to stay safe against -Wdeclaration-after-statement. D1/D2/D5 confirmed no change (audio self-adapts, OPP tiers are prose only). The remaining 15500u is only the headless rewind-bench comment, correctly untouched.
- Validation instrumentation: added an unconditional `panel: refresh_mHz=... vfp=...` line at the top of `oem diag` (register-direct, works for any running core), to sample the panel rate before/after a Genesis Region toggle.

Verified: both blob version words on disk (genesis=3, snes=2) match the bumped ABI and the loader was rebuilt against the same headers, so there is no loaderr mismatch. Artifacts staged: `out/lk_a_gba_sd_signed.img` and `out/gba_menu_boot_b.img`, both copied to `/mnt/c/pairmini`.

PENDING (needs hardware): step 29-30 flash + on-device validation. Blocked because `fastboot devices` shows no device in fastboot mode. Nothing is committed to git (no commit was requested).