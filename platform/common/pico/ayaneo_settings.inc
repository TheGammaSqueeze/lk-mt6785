/*
 * GammaOS Pico settings: every option the menus offer (volume, brightness, filters, aspect,
 * run-ahead, BFI and overdrive, resume, rewind, theme, colour, language, clock, CPU clocks), the
 * resume record, and the 192-byte block that holds them. The SD build keeps the block on the card
 * (/gammaos/settings.bin, through ayaneo_settings_serialize and _deserialize); a build without the
 * card flow keeps it in boot_b.
 *
 * Board neutral, and included as text by each board's audio file (platform/mt6785/ayaneo_audio.c,
 * platform/mt6768/block_audio.c) where it has always sat, so the board's audio code reads the
 * volume (s_gbc_vol) and the boot chime mute (s_mute_bios) as the statics of its own file. The
 * including file provides AYANEO_AUDIO_PART and AYANEO_AUDIO_VOLUME, <part_interface.h>,
 * <string.h> and arch_clean_cache_range; the hardware is reached through the display hooks
 * (ayaneo_apply_backlight, disp_pwm_strobe_mode, the overdrive setters).
 */
/* ---- persisted user settings (audio volume + screen brightness) ----
 * Stored in a single 512-byte block in boot_b at 30 MB, past the ROM (20-28 MB)
 * and the save state (28-30 MB); boot_b is 32 MB. Applied to both the boot chime
 * and the game, and to the backlight during the animation and in-game. */
/* Settings block offset in boot_b. MUST NOT be 0x01E00000 - that is where the snes9x core
 * blob lives (SNES_BLOB_OFF), and writing settings there corrupted the blob on every
 * brightness/volume/filter change (SNES then failed to load, magic read back as "ASET").
 * Moved to a dedicated 512 B slot in the boot_b tail, past all three core blobs (the old
 * non-SD boot_b layout; the SD build keeps its settings on the card). Settings are also
 * mirrored to the SD card, which stays authoritative, so this relocation loses nothing. */
#if defined(KONKR_KPA)
/* The Konkr's boot_b is 32 MB (the pairmini's 33 MB): 0x020FF000 was past its end, inside
 * vendor_boot_b, and partition_write does not stop it. The block lives in the last 4 KB now; the
 * old place is still read once if the new one is empty (AYANEO_SET_OFF_OLD). */
#define AYANEO_SET_OFF		0x01FFF000u
#define AYANEO_SET_OFF_OLD	0x020FF000u
#elif defined(KONKR_BLOCK)
/* The Konkr Pocket Block's boot_b is 32 MB as well (vendor_boot_b follows it): the last 4 KB, as on
 * the Konkr; there is no older place to read */
#define AYANEO_SET_OFF		0x01FFF000u
#else
#define AYANEO_SET_OFF		0x020FF000u	/* ~33 MB into boot_b, after the snes blob */
#endif
#define AYANEO_SET_MAGIC	0x54455341u	/* "ASET" LE */
#define AYANEO_SET_VER		10u	/* v10: software-overdrive strength (b+24 bits 19-22); ver<10 migrates to k=3 default */
#if defined(KONKR_BLOCK)
#define BLOCK_DEFAULTS_MARK	0xB1u	/* b[187]: the Konkr Pocket Block's defaults are in */
#endif
#define AYANEO_BL_MIN		16		/* keep the panel visible (never 0) */
#define AYANEO_BL_MAX		255		/* mt65xx LCD level is 0-255 */
#define AYANEO_BL_STEP		8		/* fine granularity (~30 steps) */
#ifndef AYANEO_BL_DEFAULT
#define AYANEO_BL_DEFAULT	200
#endif

extern void ayaneo_apply_backlight(int level);	/* mt_disp_drv.c: drive the LCD */
extern void disp_pwm_strobe_mode(int on);	/* ddp_pwm.c: backlight motion-blur strobe divider on/off */
extern void ayaneo_set_overdrive(int on, int kup, int kdn, int thr, int cap);	/* mt_disp_drv.c: software motion-trail overdrive */
extern void ayaneo_set_overdrive_curve(int kuphi, int kuplo, int gam);		/* mt_disp_drv.c: prev-indexed rising-gain curve */

/* All settings are persisted in the boot_b block. Field byte offsets:
 * 0 magic, 4 version, 8 volume, 12 brightness, 16 load-state-on-boot,
 * 20 skip-boot-anim/chime, 24 lcd-filter, 28 color-correction, 32 dark-filter. */
static volatile int s_gbc_vol = AYANEO_AUDIO_VOLUME;	/* audio level, 0-100 */
static volatile int s_brightness = AYANEO_BL_DEFAULT;	/* LCD level, 0-255 */
static volatile int s_load_on_boot = 1;		/* resume save state after boot */
static volatile int s_skip_boot = 0;		/* skip the animation + chime */
static volatile int s_lcd_filter[3] = { 0, 0, 0 };	/* per core [0]=SNES [1]=GBA [2]=GBC; each 0 off,1 scanlines,2 grid,3 both */
/* GBA/GBC display aspect (0=Pixel,1=Fit,2=Stretch) live in mt_disp_drv.c; persisted here by packing
 * into the FREE high bits of the b+24 filter word (bits 6-7 GBA, 8-9 GBC) so the 64-byte blob LAYOUT
 * is unchanged and old blobs (those bits 0) read back as Pixel. */
extern volatile int g_gba_aspect, g_gbc_aspect;
static volatile int s_color_correct = 0;	/* CGB colour correction on/off */
static volatile int s_dark_filter = 0;		/* 0-5 dark filter level */
static volatile int s_skip_gba_intro = 0;	/* skip the GBA BIOS boot-logo intro (SD flow) */
static volatile int s_mute_bios = 0;		/* mute the LK boot chime (boot_audio_lk) */
static volatile int s_mute_menu = 0;		/* mute the SNES menu music + SFX */
static volatile int s_show_fps = 0;		/* in-game frame rate badge (every core) */
static volatile int s_intro_black = 1;		/* the GBA BIOS intro on black (default) or white; b+44 bit 2 set = white */
static volatile int s_preempt_frames = 1;	/* run-ahead / preemptive frames (0=off, 1-3); Balanced on a new card */
static volatile unsigned s_snes_opts = 0;	/* packed snes9x menu picks: aspect | overscan<<8 |
						 * audio_interp<<16 | hires_blend<<24 (each a small index) */
static volatile int s_snes_slot = 0;		/* last-used SNES manual save-state slot (0..9) */
static volatile int s_snes_turbo = 0;		/* SNES auto-fire mask: bit0 = A, bit1 = B */
/* Genesis per-core settings, persisted in the b+24 free bits (10-17): aspect(10-11), filter(12-13),
 * region(14-15), slot(16-17). Run-ahead is shared via s_preempt_frames; volume/brightness are shared. */
static volatile int s_gen_aspect = 0;		/* 0 Pixel, 1 Fit, 2 Stretch */
static volatile int s_gen_filter = 0;		/* 0 Off, 1 Scanlines, 2 Grid, 3 Grid+ */
static volatile int s_gen_region = 0;		/* 0 Auto, 1 USA, 2 Europe, 3 Japan */
static volatile int s_gen_slot   = 0;		/* last-used Genesis manual save-state slot (0..2) */
static unsigned int rd32(const unsigned char *p);
static void wr32(unsigned char *p, unsigned int v);
/* Resume Game on Boot (default on): when a game is suspended with the power key the ROM name is
 * recorded (b+68 magic, b+72.. name) and the next boot skips the animation, the BIOS intro and
 * the menu and launches it straight away (each core reloads its own suspend state). */
static int  s_resume_on_boot = 1;
/* Rewind (every core's Pico menu), On on a new card: with it off no core takes the per frame
 * rewind snapshot (1.5 ms a frame for armGBA at 600 MHz) and the trigger does nothing. b+176:
 * 0 never written (the default, On), 1 Off, 2 On. */
static int  s_rewind_on = 1;
int  ayaneo_get_rewind(void)  { return s_rewind_on; }
void ayaneo_set_rewind(int v) { s_rewind_on = v ? 1 : 0; }
static char s_resume_name[100];
static int  s_resume_set;
#define AYANEO_RESUME_MAGIC 0x52455355u   /* "RESU" */
int  ayaneo_get_resume_on_boot(void)   { return s_resume_on_boot; }
/* Resume on Game Open: a game opened from the menu carries on from its suspend point (the state
 * saved when it was left); off, it starts from its own save. Separate from Resume on Power On
 * (s_resume_on_boot: power on straight into the game the power went off in, which always carries
 * on). The old load-on-boot flag is the built-in single game's and plays no part with the card. */
static int  s_resume_on_open = 1;
int  ayaneo_get_resume_on_open(void)   { return s_resume_on_open; }
void ayaneo_set_resume_on_open(int v)  { s_resume_on_open = v ? 1 : 0; }
/* BIOS Intro on Resume: the game Resume on Power On brings back comes after the BIOS intro, fading
 * in from the colour the intro ends on (unless Skip BIOS Intro) */
static int  s_intro_on_resume = 1;
int  ayaneo_get_intro_on_resume(void)  { return s_intro_on_resume; }
void ayaneo_set_intro_on_resume(int v) { s_intro_on_resume = v ? 1 : 0; }
void ayaneo_set_resume_on_boot(int v)  { s_resume_on_boot = v ? 1 : 0; }
int  ayaneo_resume_pending(void)       { extern void ayaneo_settings_load(void); ayaneo_settings_load(); return s_resume_on_boot && s_resume_set; }
void ayaneo_resume_mark(const char *name)
{
	unsigned i;
	if (!name || !name[0]) return;
	for (i = 0; i + 1u < sizeof s_resume_name && name[i]; i++) s_resume_name[i] = name[i];
	s_resume_name[i] = 0;
	s_resume_set = 1;
}
/* oem resumeinfo: the record as it stands, and what this boot did with it ([0] pending at the
 * GBA intro, [1] taken by the menu, [2] matched game index + 1, [3] Resume Game on Boot) */
volatile int g_dbg_resume[4];
volatile int g_dbg_set_step;   /* oem mode: 1 reading the eMMC settings, 2 parsed, 3 resume fields read */
int ayaneo_resume_peek(char *out, int cap)
{
	int i;
	if (!out || cap <= 1) return 0;
	for (i = 0; i + 1 < cap && s_resume_name[i]; i++) out[i] = s_resume_name[i];
	out[i] = 0;
	return s_resume_set;
}
void ayaneo_resume_clear(void) { s_resume_set = 0; s_resume_name[0] = 0; }   /* powering off outside a game */
/* hand the recorded name to the caller and clear the record (saved by the caller's persist) */
int ayaneo_resume_take(char *out, int cap)
{
	int i;
	if (!s_resume_set || !out || cap <= 1) return 0;
	for (i = 0; i + 1 < cap && s_resume_name[i]; i++) out[i] = s_resume_name[i];
	out[i] = 0;
	s_resume_set = 0; s_resume_name[0] = 0;
	return 1;
}
/* the menu theme: the FNV-1a hash of its name in the menu pack, 0 = the pack's first theme */
static unsigned int s_menu_theme;
unsigned int ayaneo_get_menu_theme(void)       { return s_menu_theme; }
void ayaneo_set_menu_theme(unsigned int hash)  { s_menu_theme = hash; }
/* the colour of a theme that offers colours (the Current theme), 0 = its own */
static unsigned char s_menu_colour;
int  ayaneo_get_menu_colour(void)    { return s_menu_colour; }
void ayaneo_set_menu_colour(int c)   { s_menu_colour = (unsigned char)(c < 0 ? 0 : c > 15 ? 15 : c); }
/* the menus' language (pico_i18n.c's list), 0 = English */
static unsigned char s_menu_lang;
int  ayaneo_get_language(void)       { return s_menu_lang; }
void ayaneo_set_language(int l)      { s_menu_lang = (unsigned char)(l < 0 || l > 63 ? 0 : l); }
/* the clock (pico_clock.c): the time zone as quarter hours from UTC, and the 12 hour clock */
static signed char s_tz_q;
static unsigned char s_clock12;
int  ayaneo_get_tz_quarters(void)    { return s_tz_q; }
void ayaneo_set_tz_quarters(int q)   { s_tz_q = (signed char)(q < -48 ? -48 : q > 56 ? 56 : q); }
int  ayaneo_get_clock12(void)        { return s_clock12; }
void ayaneo_set_clock12(int on)      { s_clock12 = on ? 1 : 0; }
static void resume_fields_write(unsigned char *b)
{
	unsigned i;
	wr32(b + 64, (unsigned int)(s_resume_on_boot ? 2u : 1u));   /* 0 = never written -> default on */
	wr32(b + 68, s_resume_set ? AYANEO_RESUME_MAGIC : 0u);
	for (i = 0; i < sizeof s_resume_name; i++) b[72 + i] = s_resume_set ? (unsigned char)s_resume_name[i] : 0;
	b[176] = (unsigned char)(s_rewind_on ? 2u : 1u);
	b[177] = (unsigned char)(s_menu_colour + 1u);   /* 0 = never written */
	b[178] = (unsigned char)(s_resume_on_open ? 2u : 1u);   /* 0 = never written -> on */
	b[179] = (unsigned char)(s_intro_on_resume ? 2u : 1u);  /* 0 = never written -> on */
	wr32(b + 180, s_menu_theme);
	b[184] = (unsigned char)(s_menu_lang + 1u);   /* 0 = never written -> English */
	b[185] = (unsigned char)(s_tz_q + 64);         /* 0 = never written -> UTC */
	b[186] = (unsigned char)(s_clock12 + 1u);      /* 0 = never written -> 24 hour */
#if defined(KONKR_BLOCK)
	b[187] = BLOCK_DEFAULTS_MARK;                  /* the Block's defaults are in (block_defaults_read) */
#endif
}
#if defined(KONKR_BLOCK)
/* The Konkr Pocket Block runs every core at 1800 MHz by default (its rail is raised to the
 * table's voltage for it), with one frame of run-ahead. A card written before that default
 * carries the older choices, so they are moved over once: b[187] says it has been done, and
 * whatever is chosen afterwards stays. */
static unsigned char s_cpu_manual[4] = { 7, 7, 7, 7 };   /* manual CPU clock per core, grid index + 1, 0 = automatic; 1800 on a new card */
static void block_defaults_read(const unsigned char *b, int len);
#else
static unsigned char s_cpu_manual[4] = { 5, 5, 5, 5 };   /* manual CPU clock per core, grid index + 1, 0 = automatic; 1400 (reads 1399) on a new card */
#endif
static void resume_fields_read(const unsigned char *b, int len)
{
	unsigned i;
	if (len >= 177) { if (b[176] == 2u) s_rewind_on = 1; else if (b[176] == 1u) s_rewind_on = 0; }
	if (len >= 184) { s_menu_theme = rd32(b + 180); if (b[177]) s_menu_colour = (unsigned char)((b[177] - 1u) & 15u); }
	if (len >= 184 && (b[178] == 1u || b[178] == 2u)) s_resume_on_open = (b[178] == 2u);
	if (len >= 184 && (b[179] == 1u || b[179] == 2u)) s_intro_on_resume = (b[179] == 2u);
	if (len >= 185 && b[184] && b[184] <= 64u) s_menu_lang = (unsigned char)(b[184] - 1u);
	if (len >= 186 && b[185] >= 16u && b[185] <= 120u) s_tz_q = (signed char)((int)b[185] - 64);
	if (len >= 187 && (b[186] == 1u || b[186] == 2u)) s_clock12 = (unsigned char)(b[186] - 1u);
	if (len >= 176) { s_cpu_manual[0] = b[172] & 15; s_cpu_manual[1] = b[173] & 15; s_cpu_manual[2] = b[174] & 15; s_cpu_manual[3] = b[175] & 15; }
#if defined(KONKR_BLOCK)
	block_defaults_read(b, len);
#endif
	if (len < 172) return;
	{ unsigned int r = rd32(b + 64); if (r == 1u || r == 2u) s_resume_on_boot = (r == 2u); }
	if (rd32(b + 68) == AYANEO_RESUME_MAGIC) {
		for (i = 0; i + 1u < sizeof s_resume_name; i++) s_resume_name[i] = (char)b[72 + i];
		s_resume_name[sizeof s_resume_name - 1] = 0;
		s_resume_set = s_resume_name[0] != 0;
	} else { s_resume_set = 0; s_resume_name[0] = 0; }
}
static volatile int s_strobe     = 0;		/* BFI level 0 off, 1 = 25%, 2 = 50%, 3 = 75% lit (persisted b+24 bit 18 on + bits 23:24 level) */
static volatile int s_overdrive  = 1;		/* software motion-trail overdrive on/off (persisted in b+24 bits 19-22); On applies the tuned profile */
static int s_settings_loaded;

/* ---------- little-endian helpers ---------- */
static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int rd16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static void wr32(unsigned char *p, unsigned int v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* ---------- persisted volume + brightness ---------- */

int ayaneo_gbc_audio_get_volume(void) { return s_gbc_vol; }
void ayaneo_gbc_audio_set_volume(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	s_gbc_vol = v;
}

/* apply a backlight level (clamped) to the hardware and remember it */
static void ayaneo_set_brightness(int level)
{
	if (level < AYANEO_BL_MIN) level = AYANEO_BL_MIN;
	if (level > AYANEO_BL_MAX) level = AYANEO_BL_MAX;
	s_brightness = level;
	ayaneo_apply_backlight(level);
}

/* nudge brightness by one step (dir +1/-1); returns the new level as 0-100% */
int ayaneo_brightness_step(int dir)
{
	ayaneo_set_brightness(s_brightness + dir * AYANEO_BL_STEP);
	return s_brightness * 100 / AYANEO_BL_MAX;
}
int ayaneo_brightness_pct(void) { return s_brightness * 100 / AYANEO_BL_MAX; }

/* ---- boot behaviour + video settings (all persisted) ---- */
int ayaneo_get_load_on_boot(void)   { return s_load_on_boot; }
void ayaneo_set_load_on_boot(int v) { s_load_on_boot = v ? 1 : 0; }
int ayaneo_get_skip_boot(void)      { return s_skip_boot; }
void ayaneo_set_skip_boot(int v)    { s_skip_boot = v ? 1 : 0; }
int ayaneo_get_skip_gba_intro(void)   { return s_skip_gba_intro; }
void ayaneo_set_skip_gba_intro(int v) { s_skip_gba_intro = v ? 1 : 0; }
int ayaneo_get_lcd_filter(void)     { return s_lcd_filter[0]; }           /* legacy alias -> SNES */
void ayaneo_set_lcd_filter(int v)   { s_lcd_filter[0] = (v < 0) ? 0 : (v > 5 ? 5 : v); }
int ayaneo_get_lcd_filter_core(int c)      { if (c < 0 || c > 2) c = 0; return s_lcd_filter[c]; }
void ayaneo_set_lcd_filter_core(int c, int v) { if (c < 0 || c > 2) c = 0; s_lcd_filter[c] = (v < 0) ? 0 : (v > 5 ? 5 : v); }
int ayaneo_get_color_correct(void)  { return s_color_correct; }
void ayaneo_set_color_correct(int v){ s_color_correct = v ? 1 : 0; }
int ayaneo_get_dark_filter(void)    { return s_dark_filter; }
void ayaneo_set_dark_filter(int v)  { s_dark_filter = (v < 0) ? 0 : (v > 5 ? 5 : v); }
int ayaneo_get_mute_bios(void)      { return s_mute_bios; }
void ayaneo_set_mute_bios(int v)    { s_mute_bios = v ? 1 : 0; }
int ayaneo_get_mute_menu(void)      { return s_mute_menu; }
int ayaneo_get_intro_black(void)    { return s_intro_black; }
void ayaneo_set_intro_black(int v)  { s_intro_black = v ? 1 : 0; }
int ayaneo_get_show_fps(void)       { return s_show_fps; }
void ayaneo_set_show_fps(int v)     { extern volatile int g_show_fps; s_show_fps = v ? 1 : 0; g_show_fps = s_show_fps; }
void ayaneo_set_mute_menu(int v)    { s_mute_menu = v ? 1 : 0; }
int ayaneo_get_preempt_frames(void)   { return s_preempt_frames; }
void ayaneo_set_preempt_frames(int v) { s_preempt_frames = (v < 0) ? 0 : (v > 3 ? 3 : v); }
#if defined(KONKR_BLOCK)
/* a card from before the Block's defaults: 1800 MHz on every core, one frame of run-ahead */
static void block_defaults_read(const unsigned char *b, int len)
{
	if (len >= 188 && b[187] == BLOCK_DEFAULTS_MARK)
		return;
	s_cpu_manual[0] = s_cpu_manual[1] = s_cpu_manual[2] = s_cpu_manual[3] = 7;
	ayaneo_set_preempt_frames(1);
}
#endif
unsigned ayaneo_get_snes_opts(void)   { return s_snes_opts; }
void ayaneo_set_snes_opts(unsigned v) { s_snes_opts = v; }
int  ayaneo_get_snes_slot(void)       { return s_snes_slot; }
void ayaneo_set_snes_slot(int v)      { s_snes_slot = (v < 0) ? 0 : (v > 9 ? 9 : v); }
int  ayaneo_get_snes_turbo(void)      { return s_snes_turbo; }
void ayaneo_set_snes_turbo(int v)     { s_snes_turbo = v & 3; }
int  ayaneo_get_gen_aspect(void)      { return s_gen_aspect; }
void ayaneo_set_gen_aspect(int v)     { s_gen_aspect = (v < 0) ? 0 : (v > 5 ? 0 : v); }   /* 0 Pixel 1 Core 2 Stretch 3 4:3 4 Square 5 16:9 */
int  ayaneo_get_gen_filter(void)      { return s_gen_filter; }
void ayaneo_set_gen_filter(int v)     { s_gen_filter = (v < 0) ? 0 : (v > 5 ? 5 : v); }
int  ayaneo_get_gen_region(void)      { return s_gen_region; }
void ayaneo_set_gen_region(int v)     { s_gen_region = (v < 0) ? 0 : (v > 3 ? 3 : v); }
int  ayaneo_get_gen_slot(void)        { return s_gen_slot; }
void ayaneo_set_gen_slot(int v)       { s_gen_slot = (v < 0) ? 0 : (v > 2 ? 2 : v); }
int  ayaneo_get_strobe(void)          { return s_strobe; }

/* CPU clock policy. Cores: 0 GBA, 1 GBC, 2 SNES, 3 Genesis. Every core has its own ladder of
 * clocks by run-ahead tier; the manual CPU Clock row used to set the PLL for the session only.
 * Now: a manual choice is persisted per core (b+172.. as grid index + 1, 0 = automatic) and
 * applied at every session start; otherwise the ladder value is used, raised when an LCD1x or
 * LCD3x filter is active (those cost real CPU): at least two grid steps above the core's tier 0
 * clock and one step above its current tier, whichever is higher. */
static const unsigned s_cpu_grid[8] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static int cpu_grid_idx(unsigned mhz)
{
	int i, best = 0; unsigned bd = ~0u;
	for (i = 0; i < 8; i++) { unsigned d = s_cpu_grid[i] > mhz ? s_cpu_grid[i] - mhz : mhz - s_cpu_grid[i]; if (d < bd) { bd = d; best = i; } }
	return best;
}
static unsigned cpu_step_up(unsigned mhz, int steps)
{
	int i = cpu_grid_idx(mhz) + steps;
	if (i > 7) i = 7;
	return s_cpu_grid[i] > mhz ? s_cpu_grid[i] : mhz;
}
static int core_filter_heavy(int core)
{
	int f = core == 3 ? s_gen_filter : s_lcd_filter[core == 2 ? 0 : core == 0 ? 1 : 2];
	return f >= 4;   /* 4 LCD1x, 5 LCD3x */
}
unsigned ayaneo_cpu_manual_get(int core) { return (core < 0 || core > 3 || !s_cpu_manual[core]) ? 0u : s_cpu_grid[(s_cpu_manual[core] - 1) & 7]; }
void ayaneo_cpu_manual_set(int core, unsigned mhz)
{
	if (core < 0 || core > 3) return;
	s_cpu_manual[core] = mhz ? (unsigned char)(cpu_grid_idx(mhz) + 1) : 0;
}
unsigned ayaneo_core_clock(int core, unsigned tier_mhz, unsigned tier0_mhz)
{
	unsigned m = ayaneo_cpu_manual_get(core);
	if (m) return m;
	if (core_filter_heavy(core)) {
		unsigned a = cpu_step_up(tier0_mhz, 2), b = cpu_step_up(tier_mhz, 1), lo = a > b ? a : b;
		return tier_mhz > lo ? tier_mhz : lo;
	}
	return tier_mhz;
}
/* 0 Off, 1..3 = 25/50/75 % dark with one pulse per frame, 4..6 = the same with two pulses */
const char *ayaneo_strobe_name(int b)
{
	static const char *n[4] = { "Off", "25%", "50%", "75%" };
	return n[(b < 0 || b > 3) ? 0 : b];
}
void ayaneo_set_overdrive_level(int on);
void ayaneo_set_strobe(int v)
{
	int was = s_strobe;
	s_strobe = (v < 0) ? 0 : (v > 3 ? 3 : v);
	if (!was && s_strobe) s_brightness = AYANEO_BL_MAX;   /* BFI switched on from Off: start at full brightness (the pulse is the brightness) */
	ayaneo_set_overdrive_level(s_strobe ? 1 : 0);          /* the motion overdrive goes with BFI, no separate option */
	disp_pwm_strobe_mode(s_strobe);
	ayaneo_apply_backlight(s_brightness);   /* re-apply so the strobe divider takes effect at the current level */
}
/* Tuned default overdrive profile (found by on-device sweep 2026-09-06). Falling is "free" (no halo) so
 * kdn is strong; the RISING gain is a prev-indexed curve (kuphi at dark origins = the smear killer, kuplo
 * at bright origins = the proven halo-free floor) since this panel's slow rises are dark-origin and
 * over-driving bright rises is the halo. thr/cap are guards. The menu is On/Off (the good setting is a
 * fixed recipe); power users retune live via `oem od:` / `oem odup:`. */
#if defined(KONKR_KPA)
/* Konkr ST7123, stepped through by eye 2026-11-01 (Super Mario Advance): cap 3 left a light
 * outline trailing high contrast sprites and cap 6 made it a clear halo; cap 2 removed it, kdn
 * 10 brought smear back, thr 2 was judged fine. */
#define OD_DEF_KUPHI 8
#define OD_DEF_KUPLO 2
#define OD_DEF_KDN   6
#define OD_DEF_THR   2
#define OD_DEF_CAP   2      /* the falling cap */
#define OD_DEF_CAPUP 3      /* the rising cap: 2026-11-02 four box test, rising from black was the
                             * remaining tint at 2, 4 haloed, 3 balanced; the falling side stays at 2.
                             * Rail headroom (oem odcap third value) cleared the last tint but read as
                             * washed out in games at 1 level already, so it stays off. */
#define OD_DEF_GAM   22
#else
#define OD_DEF_KUPHI 6
#define OD_DEF_KUPLO 2
#define OD_DEF_KDN   6
#define OD_DEF_THR   3
#define OD_DEF_CAP   3
#define OD_DEF_CAPUP 3
#define OD_DEF_GAM   22
#endif
int  ayaneo_get_overdrive(void)       { return s_overdrive; }
void ayaneo_set_overdrive_level(int on)
{
	s_overdrive = on ? 1 : 0;
	ayaneo_set_overdrive(s_overdrive, OD_DEF_KUPLO, OD_DEF_KDN, OD_DEF_THR, OD_DEF_CAP);   /* on/kdn/thr/cap + flat floor */
	ayaneo_set_overdrive_curve(OD_DEF_KUPHI, OD_DEF_KUPLO, OD_DEF_GAM);                    /* override rising with the curve */
	{ extern void ayaneo_set_overdrive_caps(int up, int dn, int head); ayaneo_set_overdrive_caps(OD_DEF_CAPUP, OD_DEF_CAP, 0); }
}

/* load the persisted settings from boot_b (once). Missing/invalid -> keep the
 * compile-time defaults. Does not touch the hardware; callers apply brightness. */
void ayaneo_settings_load(void)
{
	/* the whole block as ayaneo_settings_save writes it: the resume record (b+64..171), the
	 * per core CPU clocks (172..175) and Rewind (176) sit past the first 64 bytes, and reading
	 * only those lost them at every boot that did not also load the card's copy */
	static unsigned char b[512] __attribute__((aligned(64)));
	unsigned int ver;
	extern volatile int g_dbg_set_step;

	if (s_settings_loaded)
		return;
#if defined(AYANEO_GBA_SD)
	/* The SD build keeps the settings on the card only (/gammaos/settings.bin, read by the SD
	 * gate through ayaneo_settings_deserialize). Before that, and on an Android boot, the
	 * defaults stand; nothing is read from the eMMC. */
	(void)b; (void)ver; (void)g_dbg_set_step;
	s_settings_loaded = 1;
	return;
#endif
	g_dbg_set_step = 1;
	/* If storage is not up yet (early boot), leave s_settings_loaded clear so a
	 * later call retries; only commit once we have actually read the block. */
	if (partition_read(AYANEO_AUDIO_PART, AYANEO_SET_OFF, b, sizeof(b)) != (ssize_t)sizeof(b))
		return;
#ifdef AYANEO_SET_OFF_OLD
	if (rd32(b) != AYANEO_SET_MAGIC)   /* not moved yet: the next save writes the new place */
		partition_read(AYANEO_AUDIO_PART, AYANEO_SET_OFF_OLD, b, sizeof(b));
#endif
	s_settings_loaded = 1;			/* storage worked: decide once */
	if (rd32(b) != AYANEO_SET_MAGIC)
		return;				/* no/invalid blob: keep defaults */
	ver = rd32(b + 4);
	{
		int vol = (int)rd32(b + 8);
		int bl  = (int)rd32(b + 12);

		if (vol >= 0 && vol <= 100)
			s_gbc_vol = vol;
		if (bl >= AYANEO_BL_MIN && bl <= AYANEO_BL_MAX)
			s_brightness = bl;
	}
	if (ver >= 2) {			/* new fields; older blobs keep the defaults */
		s_load_on_boot  = rd32(b + 16) ? 1 : 0;
		s_skip_boot     = rd32(b + 20) ? 1 : 0;
		{ unsigned int pk = rd32(b + 24);   /* bits0-5 filters (SNES/GBA/GBC); 6-7 GBA aspect, 8-9 GBC aspect */
		  int ga, gg;
		  /* filter values 0..5: low two bits in the old places, the third bit at 25/26/27 (gen 28) */
		  ayaneo_set_lcd_filter_core(0, (int)((pk & 3) | (((pk >> 25) & 1) << 2)));
		  ayaneo_set_lcd_filter_core(1, (int)(((pk >> 2) & 3) | (((pk >> 26) & 1) << 2)));
		  ayaneo_set_lcd_filter_core(2, (int)(((pk >> 4) & 3) | (((pk >> 27) & 1) << 2)));
		  ga = (int)((pk >> 6) & 3); g_gba_aspect = (ga <= 2) ? ga : 0;   /* 3 (never written) -> Pixel */
		  gg = (int)((pk >> 8) & 3); g_gbc_aspect = (gg <= 2) ? gg : 0;
		  ayaneo_set_gen_aspect((int)(((pk >> 10) & 3) | (((pk >> 29) & 1) << 2)));   /* third bit at 29 */ ayaneo_set_gen_filter((int)(((pk >> 12) & 3) | (((pk >> 28) & 1) << 2)));
		  ayaneo_set_gen_region((int)((pk >> 14) & 3)); ayaneo_set_gen_slot((int)((pk >> 16) & 3));
		  s_strobe = ((pk >> 18) & 1) ? (int)((pk >> 23) & 3) : 0;   /* level in bits 23:24; older saves (0) -> 50% */
		  if (s_strobe == 0 && ((pk >> 18) & 1)) s_strobe = 2;
		  disp_pwm_strobe_mode(s_strobe);
		  if (ver >= 10) s_overdrive = (int)((pk >> 19) & 0xF);   /* ver<10: keep k=3 default (migration) */
		  ayaneo_set_overdrive_level(s_strobe ? 1 : 0); }   /* overdrive follows BFI */
		s_color_correct = rd32(b + 28) ? 1 : 0;
		ayaneo_set_dark_filter((int)rd32(b + 32));
	}
	if (ver >= 3)
		s_skip_gba_intro = rd32(b + 36) ? 1 : 0;
	if (ver >= 4) {
		s_mute_bios = rd32(b + 40) ? 1 : 0;
		s_mute_menu = rd32(b + 44) & 1u ? 1 : 0;
		ayaneo_set_show_fps((int)((rd32(b + 44) >> 1) & 1u));
		s_intro_black = !((rd32(b + 44) >> 2) & 1u);   /* 0 in older saves: black, the default */
	}
	if (ver >= 5)
		ayaneo_set_preempt_frames((int)rd32(b + 48));
	if (ver >= 6) {
		s_snes_opts = rd32(b + 52);
		/* v9 migration: reset ONLY the SNES audio-interpolation byte (OI_AUDIO = byte 2) to the
		 * new default (0 = Linear after the s_aud_ch reorder), preserving aspect/overscan/hires.
		 * One-time: a subsequent save stamps v9 so this no longer fires. */
		if (ver < 9)
			s_snes_opts &= ~(0xFFu << 16);
	}
	if (ver >= 7)
		ayaneo_set_snes_slot((int)rd32(b + 56));
	if (ver >= 8)
		ayaneo_set_snes_turbo((int)rd32(b + 60));
	g_dbg_set_step = 2;
	resume_fields_read(b, 192);
	g_dbg_set_step = 3;
}

/* Serialize/deserialize the settings block so a caller can persist it elsewhere
 * (the GBA-from-SD flow keeps it on the SD card). Same 64-byte layout as boot_b.
 * serialize returns the byte count; deserialize applies a block and marks the
 * settings loaded so a later boot_b load does not override the SD values. */
int ayaneo_settings_serialize(unsigned char *b, int cap)
{
	if (cap < 192) return 0;
	memset(b, 0, 192);
	wr32(b + 0, AYANEO_SET_MAGIC);
	wr32(b + 4, AYANEO_SET_VER);
	wr32(b + 8, (unsigned int)s_gbc_vol);
	wr32(b + 12, (unsigned int)s_brightness);
	wr32(b + 16, (unsigned int)s_load_on_boot);
	wr32(b + 20, (unsigned int)s_skip_boot);
	wr32(b + 24, (unsigned int)((s_lcd_filter[0] & 3) | ((s_lcd_filter[1] & 3) << 2) | ((s_lcd_filter[2] & 3) << 4)
		     | ((g_gba_aspect & 3) << 6) | ((g_gbc_aspect & 3) << 8)
		     | ((s_gen_aspect & 3) << 10) | ((s_gen_filter & 3) << 12)
		     | ((s_gen_region & 3) << 14) | ((s_gen_slot & 3) << 16)
		     | ((s_strobe ? 1u : 0u) << 18) | ((s_overdrive & 0xF) << 19) | ((unsigned int)(s_strobe ? ((s_strobe - 1) % 3) + 1 : 0) << 23) 
		     | ((unsigned int)((s_lcd_filter[0] >> 2) & 1) << 25) | ((unsigned int)((s_lcd_filter[1] >> 2) & 1) << 26)
		     | ((unsigned int)((s_lcd_filter[2] >> 2) & 1) << 27) | ((unsigned int)((s_gen_filter >> 2) & 1) << 28)
		     | ((unsigned int)((s_gen_aspect >> 2) & 1) << 29)));
	wr32(b + 28, (unsigned int)s_color_correct);
	wr32(b + 32, (unsigned int)s_dark_filter);
	wr32(b + 36, (unsigned int)s_skip_gba_intro);
	wr32(b + 40, (unsigned int)s_mute_bios);
	wr32(b + 44, (unsigned int)((s_mute_menu ? 1u : 0u) | (s_show_fps ? 2u : 0u) | (s_intro_black ? 0u : 4u)));
	wr32(b + 48, (unsigned int)s_preempt_frames);
	wr32(b + 52, s_snes_opts);
	wr32(b + 56, (unsigned int)s_snes_slot);
	wr32(b + 60, (unsigned int)s_snes_turbo);
	resume_fields_write(b);
	b[172] = s_cpu_manual[0]; b[173] = s_cpu_manual[1]; b[174] = s_cpu_manual[2]; b[175] = s_cpu_manual[3];
	return 192;
}
void ayaneo_settings_deserialize(const unsigned char *b, int len)
{
	unsigned int ver;
	if (len < 64 || rd32(b) != AYANEO_SET_MAGIC)
		return;
	ver = rd32(b + 4);
	{
		int vol = (int)rd32(b + 8), bl = (int)rd32(b + 12);
		if (vol >= 0 && vol <= 100) s_gbc_vol = vol;
		if (bl >= AYANEO_BL_MIN && bl <= AYANEO_BL_MAX) s_brightness = bl;
	}
	if (ver >= 2) {
		s_load_on_boot  = rd32(b + 16) ? 1 : 0;
		s_skip_boot     = rd32(b + 20) ? 1 : 0;
		{ unsigned int pk = rd32(b + 24);   /* bits0-5 filters (SNES/GBA/GBC); 6-7 GBA aspect, 8-9 GBC aspect */
		  int ga, gg;
		  /* filter values 0..5: low two bits in the old places, the third bit at 25/26/27 (gen 28) */
		  ayaneo_set_lcd_filter_core(0, (int)((pk & 3) | (((pk >> 25) & 1) << 2)));
		  ayaneo_set_lcd_filter_core(1, (int)(((pk >> 2) & 3) | (((pk >> 26) & 1) << 2)));
		  ayaneo_set_lcd_filter_core(2, (int)(((pk >> 4) & 3) | (((pk >> 27) & 1) << 2)));
		  ga = (int)((pk >> 6) & 3); g_gba_aspect = (ga <= 2) ? ga : 0;   /* 3 (never written) -> Pixel */
		  gg = (int)((pk >> 8) & 3); g_gbc_aspect = (gg <= 2) ? gg : 0;
		  ayaneo_set_gen_aspect((int)(((pk >> 10) & 3) | (((pk >> 29) & 1) << 2)));   /* third bit at 29 */ ayaneo_set_gen_filter((int)(((pk >> 12) & 3) | (((pk >> 28) & 1) << 2)));
		  ayaneo_set_gen_region((int)((pk >> 14) & 3)); ayaneo_set_gen_slot((int)((pk >> 16) & 3));
		  s_strobe = ((pk >> 18) & 1) ? (int)((pk >> 23) & 3) : 0;   /* level in bits 23:24; older saves (0) -> 50% */
		  if (s_strobe == 0 && ((pk >> 18) & 1)) s_strobe = 2;
		  disp_pwm_strobe_mode(s_strobe);
		  if (ver >= 10) s_overdrive = (int)((pk >> 19) & 0xF);   /* ver<10: keep k=3 default (migration) */
		  ayaneo_set_overdrive_level(s_strobe ? 1 : 0); }   /* overdrive follows BFI */
		s_color_correct = rd32(b + 28) ? 1 : 0;
		ayaneo_set_dark_filter((int)rd32(b + 32));
	}
	if (ver >= 3)
		s_skip_gba_intro = rd32(b + 36) ? 1 : 0;
	if (ver >= 4) {
		s_mute_bios = rd32(b + 40) ? 1 : 0;
		s_mute_menu = rd32(b + 44) & 1u ? 1 : 0;
		ayaneo_set_show_fps((int)((rd32(b + 44) >> 1) & 1u));
		s_intro_black = !((rd32(b + 44) >> 2) & 1u);   /* 0 in older saves: black, the default */
	}
	if (ver >= 5)
		ayaneo_set_preempt_frames((int)rd32(b + 48));
	if (ver >= 6) {
		s_snes_opts = rd32(b + 52);
		/* v9 migration: reset ONLY the SNES audio-interpolation byte (OI_AUDIO = byte 2) to the
		 * new default (0 = Linear after the s_aud_ch reorder), preserving aspect/overscan/hires.
		 * One-time: a subsequent save stamps v9 so this no longer fires. */
		if (ver < 9)
			s_snes_opts &= ~(0xFFu << 16);
	}
	if (ver >= 7)
		ayaneo_set_snes_slot((int)rd32(b + 56));
	if (ver >= 8)
		ayaneo_set_snes_turbo((int)rd32(b + 60));
	resume_fields_read(b, len);
	s_settings_loaded = 1;			/* SD values are authoritative from here */
}

/* Headless round-trip self-test for the settings blob (validates the aspect bits packed into the
 * b+24 filter word do NOT disturb the per-core LCD filters, and both survive serialize->deserialize).
 * Non-destructive: saves the live filter/aspect values, exercises the REAL serialize + deserialize
 * with distinct test values, checks they round-trip, then restores the live values. Everything else
 * in the blob is serialized from (and deserialized back to) its live value, so nothing is lost.
 * Returns 0 on pass, else a bitmask of which field failed (bit0 SNESfilt..bit4 GBCaspect). */
int ayaneo_settings_aspect_selftest(void)
{
	static unsigned char b[64];
	int of0 = s_lcd_filter[0], of1 = s_lcd_filter[1], of2 = s_lcd_filter[2];
	int oga = g_gba_aspect, ogg = g_gbc_aspect;
	int fail = 0;
	s_lcd_filter[0] = 3; s_lcd_filter[1] = 2; s_lcd_filter[2] = 1;   /* distinct test filters */
	g_gba_aspect = 2; g_gbc_aspect = 1;                             /* Stretch / Fit */
	ayaneo_settings_serialize(b, 64);
	s_lcd_filter[0] = s_lcd_filter[1] = s_lcd_filter[2] = 0;
	g_gba_aspect = g_gbc_aspect = 0;
	ayaneo_settings_deserialize(b, 64);
	if (s_lcd_filter[0] != 3) fail |= 1;
	if (s_lcd_filter[1] != 2) fail |= 2;
	if (s_lcd_filter[2] != 1) fail |= 4;
	if (g_gba_aspect   != 2) fail |= 8;
	if (g_gbc_aspect   != 1) fail |= 16;
	s_lcd_filter[0] = of0; s_lcd_filter[1] = of1; s_lcd_filter[2] = of2;   /* restore live values */
	g_gba_aspect = oga; g_gbc_aspect = ogg;
	return fail;
}

/* write all current settings back to boot_b (block-aligned); the SD build writes them to the
 * card instead (ayaneo_settings_store, gba_driver.c: /gammaos/settings.bin) */
void ayaneo_settings_save(void)
{
	static unsigned char b[512] __attribute__((aligned(64)));
#if defined(AYANEO_GBA_SD)
	{
		extern int ayaneo_settings_store(const unsigned char *blk, int len);
		int n = ayaneo_settings_serialize(b, (int)sizeof b);
		s_settings_loaded = 1;			/* our value is now authoritative */
		if (n > 0)
			ayaneo_settings_store(b, n);
		return;
	}
#endif

	memset(b, 0, sizeof(b));
	wr32(b + 0, AYANEO_SET_MAGIC);
	wr32(b + 4, AYANEO_SET_VER);
	wr32(b + 8, (unsigned int)s_gbc_vol);
	wr32(b + 12, (unsigned int)s_brightness);
	wr32(b + 16, (unsigned int)s_load_on_boot);
	wr32(b + 20, (unsigned int)s_skip_boot);
	wr32(b + 24, (unsigned int)((s_lcd_filter[0] & 3) | ((s_lcd_filter[1] & 3) << 2) | ((s_lcd_filter[2] & 3) << 4)
		     | ((g_gba_aspect & 3) << 6) | ((g_gbc_aspect & 3) << 8)
		     | ((s_gen_aspect & 3) << 10) | ((s_gen_filter & 3) << 12)
		     | ((s_gen_region & 3) << 14) | ((s_gen_slot & 3) << 16)
		     | ((s_strobe ? 1u : 0u) << 18) | ((s_overdrive & 0xF) << 19) | ((unsigned int)(s_strobe ? ((s_strobe - 1) % 3) + 1 : 0) << 23) 
		     | ((unsigned int)((s_lcd_filter[0] >> 2) & 1) << 25) | ((unsigned int)((s_lcd_filter[1] >> 2) & 1) << 26)
		     | ((unsigned int)((s_lcd_filter[2] >> 2) & 1) << 27) | ((unsigned int)((s_gen_filter >> 2) & 1) << 28)
		     | ((unsigned int)((s_gen_aspect >> 2) & 1) << 29)));
	wr32(b + 28, (unsigned int)s_color_correct);
	wr32(b + 32, (unsigned int)s_dark_filter);
	wr32(b + 36, (unsigned int)s_skip_gba_intro);
	wr32(b + 40, (unsigned int)s_mute_bios);
	wr32(b + 44, (unsigned int)((s_mute_menu ? 1u : 0u) | (s_show_fps ? 2u : 0u) | (s_intro_black ? 0u : 4u)));
	wr32(b + 48, (unsigned int)s_preempt_frames);
	wr32(b + 52, s_snes_opts);
	wr32(b + 56, (unsigned int)s_snes_slot);
	wr32(b + 60, (unsigned int)s_snes_turbo);
	resume_fields_write(b);
	b[172] = s_cpu_manual[0]; b[173] = s_cpu_manual[1]; b[174] = s_cpu_manual[2]; b[175] = s_cpu_manual[3];
	s_settings_loaded = 1;			/* our value is now authoritative */
	arch_clean_cache_range((addr_t)b, sizeof(b));
	{
		extern void ayaneo_storage_lock(void), ayaneo_storage_unlock(void);   /* sd_fat.c */
		ayaneo_storage_lock();
		partition_write(AYANEO_AUDIO_PART, AYANEO_SET_OFF, b, sizeof(b));
		ayaneo_storage_unlock();
	}
}

/* ensure settings are loaded, then push the brightness to the panel. Called by
 * the display path right after the backlight is turned on (animation frame 1,
 * or the Select-skip emulator init) so the persisted level is used from boot. */
void ayaneo_apply_persisted_brightness(void)
{
	ayaneo_settings_load();
	ayaneo_apply_backlight(s_brightness);
}

/* The persisted LCD level for mt65xx_backlight_on() to use instead of the
 * cust default, so every backlight-on during boot (there are several, in
 * platform.c / boot_mode.c) honours the saved brightness rather than snapping
 * to full. Lazily loads settings; falls back to the default until storage is up. */
int ayaneo_backlight_level_for_boot(void)
{
	ayaneo_settings_load();
	return s_brightness;
}
