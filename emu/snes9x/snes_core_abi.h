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
#define SNES_CORE_ABI_VERSION 1u

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
};

typedef const struct snes_core_exports *(*snes_core_blob_init_fn)(const struct snes_core_imports *imp);

#endif /* SNES_CORE_ABI_H */
