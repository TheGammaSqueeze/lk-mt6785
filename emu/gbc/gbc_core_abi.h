/*
 * gbc_core_abi.h - contract between LK (frontend) and the loadable gambatte GB/GBC core.
 *
 * Mirrors emu/gba/gba_core_abi.h: the gambatte core moves OUT of lk_a into a boot_b blob
 * so both cores (gpSP GBA + gambatte GB/GBC) live outside the 2 MB lk_a partition. LK reads
 * the blob from boot_b into a reserved DRAM slot (0x4E800000, clear of the gpSP blob at
 * 0x4E400000 and the shared emulator arena at 0x50000000), does cache maintenance, calls
 * the entry, and drives the core through the export table below.
 *
 * The boundary is tiny: the core calls out only for input (read_buttons) and RTC (host_time);
 * everything else (libc mem*, libgcc __aeabi_*, libm powf, the C++ runtime shim) is bundled
 * into the blob. LK drives the core through the gbc_* function pointers.
 */
#ifndef GBC_CORE_ABI_H
#define GBC_CORE_ABI_H

#define GBC_CORE_ABI_MAGIC   0x31434247u   /* "GBC1" */
#define GBC_CORE_ABI_VERSION 1u

/* Services LK provides to the core (core -> LK calls). */
struct gbc_core_imports {
	unsigned (*read_buttons)(void);   /* physical pad state -> gambatte InputGetter */
	long     (*host_time)(void);      /* wall-clock seconds for the cart RTC (time()) */
};

/* The core's surface as the LK driver uses it (LK -> core calls). Names mirror the
 * extern "C" gbc_* wrappers in gbc_wrap.cpp. */
struct gbc_core_exports {
	unsigned int magic;      /* GBC_CORE_ABI_MAGIC */
	unsigned int version;    /* GBC_CORE_ABI_VERSION */

	/* lifecycle */
	void (*heap_init)(void *base, unsigned size);   /* set the bump arena (gbc_shim.cpp) */
	int  (*create)(void);                           /* gbc_create */
	int  (*load)(const void *rom, unsigned size, unsigned flags); /* gbc_load (FORCE_DMG=1) */
	void (*reset)(void);                            /* gbc_reset */

	/* per-frame drive: video 160x144 RGB565, sound stereo @2097152 Hz */
	long (*run)(unsigned short *video, int pitch,
		    unsigned int *sound, unsigned sound_sz, unsigned *samples); /* gbc_run */

	/* cartridge battery save (.sav) + RTC blobs (persisted to SD) */
	void     *(*savedata_ptr)(void);
	unsigned  (*savedata_size)(void);
	void     *(*rtcdata_ptr)(void);
	unsigned  (*rtcdata_size)(void);

	/* save states */
	unsigned (*state_size)(void);
	void     (*state_save)(void *buf);
	int      (*state_load)(const void *buf, unsigned size);

	/* colour / palette knobs */
	void (*set_dmg_palette_color)(unsigned pal, unsigned col, unsigned rgb32);
	void (*set_color_correction)(int enable);
	void (*set_color_correction_mode)(unsigned mode);
	void (*set_dark_filter)(unsigned level);

	/* GB colorization palette catalogue (the real gambatte gbcpalettes.h list: GB,
	 * GBC, SGB, Special and the TWB64/community packs). Only meaningful for DMG (.gb)
	 * games; GBC/SGB carts colour themselves from the ROM. The frontend browses by
	 * index (count/name) and installs a palette (apply, which sets all 3 DMG palettes).
	 * default() returns the index of the standard "GBC - Dark Green" GBC palette. */
	unsigned    (*dmg_palette_count)(void);
	const char *(*dmg_palette_name)(unsigned idx);
	void        (*dmg_palette_apply)(unsigned idx);
	unsigned    (*dmg_palette_default)(void);

	/* Auto-detect: pick the per-game palette gambatte would ("internal" colorization)
	 * by matching the ROM's 16-byte header title (0x134) against gbcTitlePalettes, and
	 * install it; falls back to the default GBC palette when the game is not listed.
	 * The frontend passes the title (it holds the ROM buffer); returns the applied dir
	 * index for display, or -1 if it fell back. This is the DEFAULT palette mode. */
	int         (*dmg_palette_apply_auto)(const char *title);
};

typedef const struct gbc_core_exports *(*gbc_core_blob_init_fn)(const struct gbc_core_imports *imp);

#endif /* GBC_CORE_ABI_H */
