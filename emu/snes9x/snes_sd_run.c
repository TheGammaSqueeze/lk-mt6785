/*
 * snes_sd_run.c - run a SNES ROM from the SD card through the loadable snes9x core.
 *
 * The GBA-from-SD selector hands a chosen ROM to gba_driver.c; when it is a SNES title
 * (roms/snes) the driver calls snes_sd_session() here. We load the snes9x core blob
 * (snes_core_load, boot_b), give it a DRAM heap, load the ROM (+ its .srm) from the card,
 * then run the frame loop: each frame the core produces an RGB565 image (256/512 wide) +
 * audio; we blit it (ayaneo_snes_show_frame) paced off the 13 MHz counter. Holding AYA
 * returns to the selector. Runs on its OWN thread (deep C++ would overflow emu_thread's
 * stack - see emu/CORE_PORTING_NOTES.md item 2). Only one core runs at a time, so we reuse
 * the 0x50000000 arena.
 *
 * First bring-up: video + input. Audio and the in-game menu/run-ahead come next.
 */
#include "snes_core_abi.h"
#include "../gba/sd_fat.h"
#include "../gba/gba_sd_save.h"
#include <kernel/thread.h>
#include <kernel/event.h>

extern const struct snes_core_exports *snes_core_load(void);   /* snes_core_loader.c */
extern void     ayaneo_snes_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern void     ayaneo_gbc_audio_init(void);       /* 48 kHz AFE ring (shared with GB/GBC) */
extern void     ayaneo_snes_audio_reset(void);
extern void     ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_display_prepare(void);
extern unsigned gpt4_get_current_tick(void);
extern void     mtk_wdt_restart(void);
extern void     mtk_wdt_disable(void);
extern int      mt_get_gpio_in(unsigned pin);

/* pad GPIOs (match gba_driver.c). Active-low. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#define GPIO_AYA       86
#define GPIO_LB        92
#define GPIO_RB        81
#define GPIO_SELECT    90
#define GPIO_START     91
#define GPIO_B         82
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#define GPIO_A         83
#define GPIO_X         85      /* physical X (84/85 read swapped vs the caps) */
#define GPIO_Y         84      /* physical Y */

/* RETRO_DEVICE_ID_JOYPAD_* bit positions (what the core's input_state_cb reads). */
#define RJ_B 0
#define RJ_Y 1
#define RJ_SELECT 2
#define RJ_START 3
#define RJ_UP 4
#define RJ_DOWN 5
#define RJ_LEFT 6
#define RJ_RIGHT 7
#define RJ_A 8
#define RJ_X 9
#define RJ_L 10
#define RJ_R 11

/* Physical pad -> SNES button bitmask (imports.read_buttons). */
unsigned ayaneo_snes_pad_mask(void)
{
	unsigned m = 0;
	if (PRESSED(GPIO_B))      m |= 1u << RJ_B;
	if (PRESSED(GPIO_Y))      m |= 1u << RJ_Y;
	if (PRESSED(GPIO_SELECT)) m |= 1u << RJ_SELECT;
	if (PRESSED(GPIO_START))  m |= 1u << RJ_START;
	if (PRESSED(GPIO_UP))     m |= 1u << RJ_UP;
	if (PRESSED(GPIO_DOWN))   m |= 1u << RJ_DOWN;
	if (PRESSED(GPIO_LEFT))   m |= 1u << RJ_LEFT;
	if (PRESSED(GPIO_RIGHT))  m |= 1u << RJ_RIGHT;
	if (PRESSED(GPIO_A))      m |= 1u << RJ_A;
	if (PRESSED(GPIO_X))      m |= 1u << RJ_X;
	if (PRESSED(GPIO_LB))     m |= 1u << RJ_L;
	if (PRESSED(GPIO_RB))     m |= 1u << RJ_R;
	return m;
}

/* Arena (0x50000000, 64 MB, shared - only one core runs at a time). */
#define SNES_ROM_BUF   0x50000000u
#define SNES_ROM_CAP   0x00800000u        /* 8 MB (largest SNES carts ~6 MB) */
#define SNES_HEAP_BASE 0x50800000u
#define SNES_HEAP_SZ   0x03000000u        /* 48 MB for snes9x's internal allocations */

static void snes_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct snes_core_exports *c;
	unsigned char *rombuf = (unsigned char *)SNES_ROM_BUF;
	unsigned romsz;
	unsigned bw = 0, bh = 0, mw = 0, mh = 0, sr = 0;
	int aya_hold = 0;
	unsigned pace_base;
	unsigned long long pace_n = 0;
	/* SNES runs at its native 60.0988 Hz (NTSC), NOT the 59.7275 Hz the GB/GBC/GBA cores
	 * pace to. Ticks/frame @13 MHz = 13000000 / 60.0988 = 216309.5; express as a num/den
	 * so the cumulative target (pace_base + pace_n*NUM/DEN) has no drift. */
	const unsigned long long TPF_NUM = 130000000000ull;   /* 13000000 * 10000 */
	const unsigned long long TPF_DEN = 600988ull;         /* 60.0988 Hz * 10000 */

	c = snes_core_load();
	if (!c) return;

	romsz = gba_sd_load_rom(vol, rom, rombuf, SNES_ROM_CAP);
	if (!romsz) return;

	c->heap_init((void *)SNES_HEAP_BASE, SNES_HEAP_SZ);
	c->init();
	if (c->load(rombuf, romsz) != 0) return;
	c->av_info(&bw, &bh, &mw, &mh, &sr);

	/* cartridge battery save (.srm) from the card, if any. */
	{
		unsigned sz = c->sram_size();
		void *p = c->sram_ptr();
		if (p && sz && sz <= 0x20000u)   /* SNES SRAM max 128 KB */
			gba_sd_read_named(vol, "/saves/snes", rom->name, "srm", (unsigned char *)p, sz);
	}

	ayaneo_display_prepare();
	ayaneo_gbc_audio_init();
	ayaneo_snes_audio_reset();
	mtk_wdt_disable();

	pace_base = gpt4_get_current_tick();
	for (;;) {
		struct snes_frame f;
		int aya = PRESSED(GPIO_AYA);
		mtk_wdt_restart();

		c->run(&f);
		if (f.video && f.width && f.height)
			ayaneo_snes_show_frame((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
		if (f.audio && f.frames)
			ayaneo_snes_audio_submit(f.audio, f.frames, sr ? sr : 32040u);

		if (aya) { if (++aya_hold >= 60) break; } else aya_hold = 0;

		pace_n++;
		{
			unsigned target = pace_base + (unsigned)(pace_n * TPF_NUM / TPF_DEN);
			while ((int)(gpt4_get_current_tick() - target) < 0)
				;
		}
	}

	/* persist SRAM on exit */
	{
		unsigned sz = c->sram_size();
		void *p = c->sram_ptr();
		if (p && sz && sz <= 0x20000u)
			gba_sd_write_named(vol, "/saves/snes", rom->name, "srm", p, sz);
	}
	c->unload();
	ayaneo_menu_audio_silence();
}

/* ---- dedicated emulation thread (see CORE_PORTING_NOTES item 2) ---- */
static thread_t *s_snes_thread;
static event_t   s_snes_kick, s_snes_done;
static fat_vol            *s_snes_vol;
static const gba_rom_entry *s_snes_rom;

static int snes_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_snes_kick);
		snes_session_body(s_snes_vol, s_snes_rom);
		event_signal(&s_snes_done, false);
	}
	return 0;
}

void snes_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	s_snes_vol = vol; s_snes_rom = rom;
	if (!s_snes_thread) {
		event_init(&s_snes_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_snes_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_snes_thread = thread_create("snes_emu", snes_thread_fn, 0, DEFAULT_PRIORITY, 262144);
		thread_resume(s_snes_thread);
	}
	event_signal(&s_snes_kick, true);
	event_wait(&s_snes_done);
}
