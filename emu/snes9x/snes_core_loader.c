/*
 * snes_core_loader.c - LK side of the loadable SNES cores (see snes_core_abi.h): snes9x, or armSNES
 * when the player picked it (the in-game menu's SNES Core row), as the GBA has armGBA and gpSP.
 *
 * Compiled INTO lk_a. snes_core_load() copies the core from the SD card (CORE_PATH_SNES9X or
 * CORE_PATH_ARMSNES, core_file.h) into its fixed DRAM slot, calls the blob's entry with the imports
 * table and returns the export table the session drives the core through. Both fill the same table.
 */
#include "snes_core_abi.h"
#include "../gba/core_file.h"
#include "../gba/gba_sd_save.h"   /* the SNES Core choice on the card */
#include <string.h>

/* imports the core calls back into LK for */
extern unsigned ayaneo_snes_pad_mask(void);   /* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits */
extern long gba_host_time(void);               /* wall-clock seconds for SRTC / BS-X carts */

static const struct snes_core_imports s_imports = {
	ayaneo_snes_pad_mask,
	gba_host_time,
};

#define CORE_SLOT_VMA  0x4F000000u
#define CORE_SLOT_END  0x4F800000u
/* armSNES's picture buffers and memory maps are static (10 MB with them): it takes the Genesis and
 * NES cores' slot too, up to the emulator arena. Only one core runs at a time. */
#define ARMSNES_SLOT_END 0x50000000u

/* diagnostics for `fastboot oem diag`: 0 ok, else the core_file.h CORE_FILE_* reason */
volatile unsigned g_snes_dbg_loaderr;

/* ---- which SNES core: snes9x, or armSNES when picked (the SNES Core row) ----
 * The choice is kept in /gammaos/snescore.cfg ("armsnes" or "snes9x"); without it, snes9x. A game
 * starts with the other core when the chosen one is not on the card or does not load. */
#define PREF_DIR  "/gammaos"
#define PREF_NAME "snescore"
static int s_pref = -1;          /* -1 not read yet, 0 snes9x, 1 armSNES */
static int s_loaded_armsnes;     /* the core loaded last is armSNES */
static int s_present[2] = { -1, -1 };   /* each core's file on the card: -1 not looked, 0 no, 1 yes */

static int pref(fat_vol *v)
{
	if (s_pref < 0) {
		char b[16];
		unsigned n = v ? gba_sd_read_named(v, PREF_DIR, PREF_NAME, "cfg", (unsigned char *)b, sizeof b - 1) : 0;
		b[n] = 0;
		s_pref = n >= 7 && !memcmp(b, "armsnes", 7);
	}
	return s_pref;
}
static int present(fat_vol *v, int armsnes)
{
	if (s_present[armsnes] < 0) {
		fat_file f;
		s_present[armsnes] = v && fat_open(v, armsnes ? CORE_PATH_ARMSNES : CORE_PATH_SNES9X, &f) == 0;
	}
	return s_present[armsnes];
}
/* the core the next game starts with */
int snes_core_next_armsnes(fat_vol *v)
{
	int want = pref(v);
	return present(v, want) || !present(v, !want) ? want : !want;
}
int snes_core_pref_armsnes(fat_vol *v) { return pref(v); }
int snes_core_armsnes_present(fat_vol *v) { return present(v, 1); }
int snes_core_is_armsnes(void) { return s_loaded_armsnes; }
/* the SNES Core row: the choice, kept on the card (off the game's thread) */
void snes_core_set_pref(fat_vol *v, int armsnes)
{
	static const char a[] = "armsnes\n", b[] = "snes9x\n";
	s_pref = armsnes != 0;
	if (v) gba_sd_write_named_bg(v, PREF_DIR, PREF_NAME, "cfg", s_pref ? a : b, s_pref ? sizeof a - 1 : sizeof b - 1);
}
/* the folder a core's save states are in: they do not load in the other */
const char *snes_core_state_dir(int armsnes) { return armsnes ? "/states/armsnes" : "/states/snes"; }

static const struct snes_core_exports *load_one(fat_vol *v, int armsnes)
{
	snes_core_blob_init_fn init;
	const struct snes_core_exports *ex;
	int err;

	init = (snes_core_blob_init_fn)core_file_load(v, armsnes ? CORE_PATH_ARMSNES : CORE_PATH_SNES9X, CORE_SLOT_VMA,
						      (armsnes ? ARMSNES_SLOT_END : CORE_SLOT_END) - CORE_SLOT_VMA,
						      SNES_CORE_ABI_MAGIC, 2u, SNES_CORE_ABI_V6, &err);   /* v2 cores: no netplay */
	g_snes_dbg_loaderr = (unsigned)err;
	if (!init)
		return 0;
	ex = init(&s_imports);
	if (!ex || ex->magic != SNES_CORE_ABI_MAGIC || ex->version < 2u || ex->version > SNES_CORE_ABI_V6) {
		g_snes_dbg_loaderr = CORE_FILE_REJECTED;
		return 0;
	}
	return ex;
}

/* the chosen core, else the other one; 0 when neither loads. A netplay session runs snes9x on both
 * consoles whatever each has picked: the two machines step in lockstep, so both must be the same
 * core (snes9x first, armSNES only when snes9x is not on the card). */
const struct snes_core_exports *snes_core_load(fat_vol *v, int netplay)
{
	int want = netplay ? !present(v, 0) : snes_core_next_armsnes(v);
	const struct snes_core_exports *ex = load_one(v, want);
	if (!ex) { want = !want; ex = load_one(v, want); }
	s_loaded_armsnes = ex && want;
	return ex;
}
