/*
 * genesis_core_abi.h - contract between LK (frontend) and the loadable Genesis-Plus-GX core.
 *
 * Mirrors emu/snes9x/snes_core_abi.h: the GPGX core (Sega Genesis/MD, Master System, Game
 * Gear, SG-1000) lives in a boot_b blob loaded into a reserved DRAM slot, driven through the
 * export table below. The core is the libretro Genesis-Plus-GX build; these exports wrap its
 * retro_* API (the blob installs the libretro callbacks internally and pumps retro_run per
 * frame). Only ONE core runs at a time; GPGX reuses the shared emulator arena at 0x50000000.
 *
 * The frontend selects the emulated system by ROM extension before load() (see genesis_system
 * values); GPGX auto-detects most from the ROM but the frontend forces SMS/GG/SG when the file
 * came from that console's folder.
 */
#ifndef GENESIS_CORE_ABI_H
#define GENESIS_CORE_ABI_H

#define GENESIS_CORE_ABI_MAGIC   0x31474553u   /* "SEG1" */
#define GENESIS_CORE_ABI_VERSION 3u   /* v3: + fps_milli (core frame rate for dynamic per-core panel refresh; v2 was set_ra_fast, sound_rebase) */

/* System hint passed to load() so the core forces the right machine for ambiguous files
 * (.bin/.sg can be MD or SMS or SG). 0 = auto-detect from the ROM header. */
enum genesis_system {
	GEN_SYS_AUTO = 0,
	GEN_SYS_MD   = 1,   /* Genesis / Mega Drive */
	GEN_SYS_SMS  = 2,   /* Master System (Mark III) */
	GEN_SYS_GG   = 3,   /* Game Gear */
	GEN_SYS_SG   = 4    /* SG-1000 */
};

/* Services LK provides to the core (core -> LK). */
struct genesis_core_imports {
	unsigned (*read_buttons)(unsigned port);   /* pad state, RETRO_DEVICE_ID_JOYPAD_* bits, port 0/1 */
	long     (*host_time)(void);               /* wall-clock seconds (unused by GPGX, kept for parity) */
};

/* One presented frame + its audio, returned by run(). video is RGB565. */
struct genesis_frame {
	const void  *video;    /* RGB565 pixels, or NULL if the frame was dropped */
	unsigned     width;    /* 256/320 (H32/H40); GG cropped 160; SMS/SG 256 */
	unsigned     height;   /* 192/224/240, or 448/480 interlaced */
	unsigned     pitch;    /* bytes per row */
	const short *audio;    /* interleaved stereo s16 */
	unsigned     frames;   /* audio frame count (stereo pairs) */
};

struct genesis_core_exports {
	unsigned int magic;      /* GENESIS_CORE_ABI_MAGIC */
	unsigned int version;    /* GENESIS_CORE_ABI_VERSION */

	/* lifecycle */
	void (*heap_init)(void *base, unsigned size);       /* bump arena (genesis_shim.c) */
	void (*init)(void);                                 /* install callbacks + retro_init */
	int  (*load)(const void *rom, unsigned size, int system);   /* retro_load_game (buffer); 0 = ok */
	void (*reset)(void);                                /* retro_reset */
	void (*unload)(void);                               /* retro_unload_game + retro_deinit */

	/* per-frame drive: pump one frame, fill *out with video + audio */
	void (*run)(struct genesis_frame *out);

	/* geometry / timing (post-load) */
	void (*av_info)(unsigned *base_w, unsigned *base_h,
			unsigned *max_w, unsigned *max_h, unsigned *sample_rate);

	/* cartridge battery save (SRAM / EEPROM) */
	void     *(*sram_ptr)(void);
	unsigned  (*sram_size)(void);

	/* save states (retro_serialize / retro_unserialize) */
	unsigned (*state_size)(void);
	int      (*state_save)(void *buf, unsigned size);
	int      (*state_load)(const void *buf, unsigned size);

	/* debug: the emulated 68000 (or Z80 for 8-bit) program counter, to see whether the CPU
	 * is running game code or stuck. */
	unsigned (*dbg_pc)(void);
	/* debug: read a blob-side diagnostic counter by index (see genesis_dbg_get). */
	unsigned (*dbg_get)(unsigned idx);

	/* core options: set a libretro option by key (e.g. "genesis_plus_gx_aspect_ratio"="4:3").
	 * GPGX re-reads it on the next run(). 0 = ok. */
	int (*set_option)(const char *key, const char *value);
	/* current display aspect ratio * 1000 (e.g. 1333 = 4:3), from retro av_info geometry. */
	unsigned (*aspect_x1000)(void);

	/* bump-arena mark/reset: reclaim any temporary buffers the core allocates and never frees
	 * so per-frame run-ahead save/load does not exhaust the arena. Mark before state_save,
	 * reset after state_load. (GPGX allocates almost nothing per serialize, but kept for parity.) */
	void *(*heap_mark)(void);
	void  (*heap_reset)(void *mark);

	/* run-ahead render/audio skip: set before a run() to skip the video render (skip_video) and/or
	 * hard-disable audio (skip_audio) for look-ahead frames never presented/heard. Both cleared
	 * (0,0) for normal frames. */
	void  (*set_av_skip)(int skip_video, int skip_audio);

	/* run-ahead FAST_SAVESTATES: when on (1), retro_serialize/unserialize ALSO save/restore the
	 * blip-buffer + FM/CD audio-phase side-channel and suppress the state_load blip_clear, so a
	 * state_save/state_load PAIR is audio-continuous. Scope it to the run-ahead save/load pair only
	 * (the side-channel is a single global latch; leaving it on globally lets per-frame ring captures
	 * clobber it). Off for portable SD/manual/suspend saves. */
	void  (*set_ra_fast)(int on);
	/* re-baseline the audio synthesis (blip_clear all channels + zero the FM last-level carry) so the
	 * NEXT run() starts from a clean, consistent phase. Called after a rewind state_load (which is NOT
	 * fast, so it cannot restore the per-entry phase) to stop each rewound frame stepping against a
	 * stale integrator = the reverse-audio buzz/loop. */
	void  (*sound_rebase)(void);

	/* current emulated frame rate in milli-Hz (e.g. 59923 = 59.923 Hz NTSC, 49701 = 49.701 Hz PAL),
	 * read live from retro av_info. GPGX recomputes this on a region change (get_region updates
	 * system_clock + vdp_pal + lines_per_frame; fps = system_clock/lines_per_frame/MCYCLES_PER_LINE),
	 * but NEVER fires SET_SYSTEM_AV_INFO, so LK must POLL this after the region option takes effect
	 * and retune the panel vfp. */
	unsigned (*fps_milli)(void);
};

typedef const struct genesis_core_exports *(*genesis_core_blob_init_fn)(const struct genesis_core_imports *imp);

#endif /* GENESIS_CORE_ABI_H */
