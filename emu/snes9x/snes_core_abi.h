/*
 * snes_core_abi.h - contract between LK (frontend) and the loadable snes9x core.
 *
 * Mirrors emu/gbc/gbc_core_abi.h: the snes9x core lives in a boot_b blob loaded into a
 * reserved DRAM slot, driven through the export table below. The core itself is the
 * libretro snes9x build; these exports wrap its retro_* API (the blob installs the
 * libretro callbacks internally and pumps retro_run per frame). Only ONE core runs at a
 * time; snes9x reuses the shared emulator arena at 0x50000000 for its heap.
 */
#ifndef SNES_CORE_ABI_H
#define SNES_CORE_ABI_H

#define SNES_CORE_ABI_MAGIC   0x31534E53u   /* "SNS1" */
#define SNES_CORE_ABI_VERSION 4u   /* v2: + fps_milli (core frame rate for dynamic per-core panel refresh)
				    * v3: + set_pad_reader (both controller ports, for netplay)
				    * v4: + opt_count / opt_info (the core's own option list) */
#define SNES_CORE_ABI_V5      5u   /* v5: + get_memory (the machine's memory, for RetroAchievements) */
#define SNES_CORE_ABI_V6      6u   /* v6: + cheat codes */

#include "pico_core_memory.h"

/* One of the core's own options, as the core declares it (libretro core options v2), for the
 * frontend's Core Settings page. values are what set_option takes, labels what is shown. Shared
 * with the Genesis and NES ABIs. */
#ifndef PICO_CORE_OPTION_DEFINED
#define PICO_CORE_OPTION_DEFINED
struct pico_core_option {
	const char *key;                 /* set_option's key, and the key the frontend saves it under */
	const char *label;               /* the option's name */
	const char *category;            /* its group's name, or 0 */
	const char *const *values;       /* set_option values */
	const char *const *labels;       /* what each value is called */
	int nvalues;
	int def;                         /* the core's default (an index) */
};
#endif

/* Services LK provides to the core (core -> LK). */
struct snes_core_imports {
	unsigned (*read_buttons)(void);   /* pad state as RETRO_DEVICE_ID_JOYPAD_* bit positions */
	long     (*host_time)(void);      /* wall-clock seconds (SRTC / real-time-clock carts) */
};

/* One presented frame + its audio, returned by run(). video is RGB565. */
struct snes_frame {
	const void  *video;    /* RGB565 pixels, or NULL if the frame was dropped */
	unsigned     width;    /* 256 or 512 (hi-res) */
	unsigned     height;   /* 224/239 or 448/478 (interlace) */
	unsigned     pitch;    /* bytes per row */
	const short *audio;    /* interleaved stereo s16 */
	unsigned     frames;   /* audio frame count (stereo pairs) */
};

struct snes_core_exports {
	unsigned int magic;      /* SNES_CORE_ABI_MAGIC */
	unsigned int version;    /* SNES_CORE_ABI_VERSION */

	/* lifecycle */
	void (*heap_init)(void *base, unsigned size);   /* bump arena (snes_shim.cpp) */
	void (*init)(void);                             /* install callbacks + retro_init */
	int  (*load)(const void *rom, unsigned size);   /* retro_load_game (buffer); 0 = ok */
	void (*reset)(void);                            /* retro_reset */
	void (*unload)(void);                           /* retro_unload_game + retro_deinit */

	/* per-frame drive: pump one frame, fill *out with video + audio */
	void (*run)(struct snes_frame *out);

	/* geometry / timing (post-load) */
	void (*av_info)(unsigned *base_w, unsigned *base_h,
			unsigned *max_w, unsigned *max_h, unsigned *sample_rate);

	/* cartridge battery save (SRAM) */
	void     *(*sram_ptr)(void);
	unsigned  (*sram_size)(void);

	/* save states */
	unsigned (*state_size)(void);
	int      (*state_save)(void *buf, unsigned size);
	int      (*state_load)(const void *buf, unsigned size);

	/* debug: the emulated 65816 program counter (Registers.PBPC), to see whether the CPU
	 * is running game code (wide PC range) or stuck in a tight spin (e.g. APU handshake). */
	unsigned (*dbg_pc)(void);

	/* debug: read a blob-side diagnostic counter by index (see snes_dbg_get in
	 * snes_core_exports.cpp). Appended at struct end so the magic/version check is
	 * unaffected; LK and the blob are always rebuilt together. */
	unsigned (*dbg_get)(unsigned idx);

	/* core options: set a libretro option by key (e.g. "snes9x_aspect"="4:3"). snes9x
	 * re-reads it on the next run() (geometry reflows for aspect/overscan). 0 = ok. */
	int (*set_option)(const char *key, const char *value);
	/* current display aspect ratio * 1000 (e.g. 1333 = 4:3), from retro av_info geometry;
	 * LK's blit stretches to honour it (see ayaneo_snes_show_frame). */
	unsigned (*aspect_x1000)(void);

	/* bump-arena mark/reset: reclaim the temporary buffers snes9x's serialize/unserialize
	 * allocate (and never free) so per-frame run-ahead save/load does not exhaust the arena.
	 * Mark before state_save, reset after state_load. */
	void *(*heap_mark)(void);
	void  (*heap_reset)(void *mark);

	/* run-ahead fast savestates: LK sets this around the per-frame save/load so snes9x
	 * serializes with the fast (direct-memory) path; cleared for portable SD saves. */
	void  (*set_ra_fast)(int on);

	/* run-ahead render/audio skip: set before a run() to skip the PPU render (skip_video) and/or
	 * hard-disable audio (skip_audio) for look-ahead frames that are never presented or heard.
	 * This is the core of the run-ahead speedup: only the presented frame needs video, only the
	 * committed frame needs audio. Both cleared (0,0) for normal frames. */
	void  (*set_av_skip)(int skip_video, int skip_audio);

	/* run-ahead RAW in-memory snapshot: much faster than state_save/state_load (no big-endian
	 * byte-swap, no per-struct alloc, copies only the cart's real SRAM extent). NOT a portable
	 * save format - LK uses it ONLY for the transient run-ahead save/rewind. state_save_ra returns
	 * bytes written, or 0 if unsupported (special-chip cart) so the caller falls back to state_save;
	 * state_load_ra returns 0 on success, -1 if unsupported. */
	unsigned (*state_save_ra)(void *buf, unsigned cap);
	int      (*state_load_ra)(const void *buf);

	/* emulated native refresh in milliHz (e.g. 60099 = 60.0988 Hz NTSC, 50007 = 50.0070 Hz PAL),
	 * from retro av.timing.fps * 1000. Region is decided at load (ROM auto-detect / snes9x_region),
	 * so this is valid post-load and re-poll-able. LK derives the panel vfp from it. */
	unsigned (*fps_milli)(void);
	/* v3: both controller ports. With a reader set the core asks it for port 0 and port 1
	 * (RETRO_DEVICE_ID_JOYPAD_* bits); without one (an older frontend never sets it) it reads
	 * imports.read_buttons for port 0 only, as before. NULL clears it. */
	void (*set_pad_reader)(unsigned (*reader)(unsigned port));

	/* v4: the options the core declared, less the ones the LK's own rows set (aspect, overscan,
	 * audio interpolation, hi-res blending) and the light gun ones. Changed with set_option. */
	int (*opt_count)(void);
	const struct pico_core_option *(*opt_info)(int index);

	/* v5: the game's memory (the console's work RAM and the cartridge's RAM, as libretro's memory
	 * calls give them; no map), for RetroAchievements to read in place between frames. Valid from
	 * load until unload. */
	void (*get_memory)(struct pico_core_memory *out);

	/* v6: cheat codes. cheat_reset drops every code in force; cheat_add adds one cheat's code as a
	 * RetroArch .cht file gives it (one or more codes joined by '+' or spaces), in force from the
	 * next frame: 0 = taken, negative = a code the core cannot read (the others in the same text
	 * are taken). Between frames, from load until unload. */
	void (*cheat_reset)(void);
	int  (*cheat_add)(const char *code);
};

typedef const struct snes_core_exports *(*snes_core_blob_init_fn)(const struct snes_core_imports *imp);

#endif /* SNES_CORE_ABI_H */
