/*
 * nes_core_abi.h - the contract between the LK (frontend) and the loadable armNES core.
 *
 * armNES (NES, Famicom and Famicom Disk System) runs as a blob the LK copies from the card into a
 * fixed DRAM slot and drives through the export table below, as the Genesis core is driven
 * (genesis_core_abi.h). The blob is armNES's libretro build: these exports wrap its retro_* calls,
 * with the libretro callbacks installed inside the blob. Only one core runs at a time; the NES core
 * uses the Genesis core's slot and the shared emulator arena.
 *
 * The LK hands the core whole images: it reads the game from the card (unpacking a .zip itself) and,
 * for a Disk System game, the BIOS (disksys.rom) too. The core opens no files.
 *
 * Picture: RGB565. Sound: 48000 Hz stereo. Frame rate: NTSC 60.0988 Hz, PAL and Dendy 50.007 Hz
 * (fps_milli). Disk System: L3 on pad 0 takes the disk out, then puts the next side in.
 */
#ifndef NES_CORE_ABI_H
#define NES_CORE_ABI_H

#define NES_CORE_ABI_MAGIC   0x3153454Eu   /* "NES1" */
#define NES_CORE_ABI_VERSION 1u

/* Services the LK gives the core (core -> LK). */
struct nes_core_imports {
	unsigned (*read_buttons)(unsigned port);   /* pad state as RETRO_DEVICE_ID_JOYPAD_* bits, port 0..3 */
	long     (*host_time)(void);               /* wall clock seconds (for a cart's clock chip, if any) */
};

/* One frame's picture and sound, filled by run(). */
struct nes_frame {
	const void  *video;    /* RGB565, or NULL when the frame was not drawn (set_av_skip) */
	unsigned     width;    /* 256 less any side crop (wider with the NTSC filter) */
	unsigned     height;   /* 240 less any top and bottom crop */
	unsigned     pitch;    /* bytes per row */
	const short *audio;    /* interleaved stereo s16 */
	unsigned     frames;   /* stereo frames */
};

struct nes_core_exports {
	unsigned int magic;      /* NES_CORE_ABI_MAGIC */
	unsigned int version;    /* NES_CORE_ABI_VERSION */

	/* lifecycle */
	void (*heap_init)(void *base, unsigned size);   /* the core's heap: the arena the LK gives it */
	void (*init)(void);                             /* install the callbacks, retro_init */
	/* retro_load_game on the image in rom[0, size). bios: disksys.rom for a Disk System game (8 KiB),
	 * else NULL. is_disk: the image is an .fds (the frontend's file type), 0 for a cartridge.
	 * 0 = loaded; negative = refused (bad image, a Disk System game without the BIOS). */
	int  (*load)(const void *rom, unsigned size, const void *bios, unsigned bios_size, int is_disk);
	void (*reset)(void);                            /* retro_reset */
	void (*unload)(void);                           /* retro_unload_game, retro_deinit */

	/* one emulated frame */
	void (*run)(struct nes_frame *out);

	/* geometry and timing, after load */
	void (*av_info)(unsigned *base_w, unsigned *base_h,
			unsigned *max_w, unsigned *max_h, unsigned *sample_rate);
	unsigned (*fps_milli)(void);        /* the frame rate in milli-Hz, read live (a region change moves it) */
	unsigned (*aspect_x1000)(void);     /* the display aspect ratio * 1000 (armnes_aspect) */

	/* the battery save: cartridge RAM, or the whole disk for a Disk System game (its writes) */
	void     *(*sram_ptr)(void);
	unsigned  (*sram_size)(void);

	/* save states (retro_serialize, retro_unserialize); 0 = done */
	unsigned (*state_size)(void);
	int      (*state_save)(void *buf, unsigned size);
	int      (*state_load)(const void *buf, unsigned size);

	/* core options by libretro key ("armnes_region" = "PAL", ...), read again at the next run();
	 * 0 = taken */
	int (*set_option)(const char *key, const char *value);

	/* run ahead and fast forward: before a run(), skip_video 1 = do not draw the picture (a frame
	 * nobody sees; what the game reads back of the PPU still happens), skip_audio 1 = make no sound.
	 * (0, 0) for a normal frame. */
	void (*set_av_skip)(int skip_video, int skip_audio);

	/* the heap's high water mark and a reset to it: run ahead's save and load pair must not leave
	 * temporaries behind (mark before state_save, reset after state_load) */
	void *(*heap_mark)(void);
	void  (*heap_reset)(void *mark);

	/* debug: the emulated 6502's program counter, and a counter of the glue's by index */
	unsigned (*dbg_pc)(void);
	unsigned (*dbg_get)(unsigned idx);
};

typedef const struct nes_core_exports *(*nes_core_blob_init_fn)(const struct nes_core_imports *imp);

#endif /* NES_CORE_ABI_H */
