/*
 * pico_settings.c - the settings every menu theme offers (pico_settings.h).
 */
#include <stdio.h>
#include "pico_settings.h"
#include "pico_i18n.h"
#include "pico_clock.h"
#include "pico_version.h"

extern int  ayaneo_brightness_pct(void);
extern void ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_get_strobe(void);
extern void ayaneo_set_strobe(int v);
extern const char *ayaneo_strobe_name(int v);
extern int  ayaneo_get_skip_gba_intro(void);
extern void ayaneo_set_skip_gba_intro(int v);
extern int  ayaneo_get_intro_black(void);
extern void ayaneo_set_intro_black(int v);
extern int  ayaneo_get_mute_bios(void);
extern void ayaneo_set_mute_bios(int v);
extern int  ayaneo_get_mute_menu(void);
extern void ayaneo_set_mute_menu(int v);
extern int  ayaneo_get_intro_on_resume(void);
extern void ayaneo_set_intro_on_resume(int v);
extern int  ayaneo_get_resume_on_boot(void);
extern void ayaneo_set_resume_on_boot(int v);
extern int  ayaneo_get_resume_on_open(void);
extern void ayaneo_set_resume_on_open(int v);
extern void ayaneo_menu_settings_persist(void);
extern void ayaneo_boot_to_os(void);

static const char *const k_name[PS_N] = { "Brightness", "Volume", "Black Frame Insertion", "Theme", "Colour",
	"BIOS Boot", "Game Resume", "Boot to Android",
	"Skip BIOS Intro", "BIOS Intro Background", "Mute BIOS Audio", "BIOS Intro on Resume",
	"Resume on Power On", "Resume on Game Open", "Language", "Date & Time", "Year", "Month", "Day",
	"Hour", "Minute", "Time zone", "24-hour clock", "About", "Mute Menu Audio" };
static const char *const k_about[PS_N] = { 0, 0, 0, 0, 0,
	"The Game Boy Advance start-up screen", "Where games carry on from", "Restart into Android",
	"Start games without the logo screen", "Behind the logo at start-up", "The start-up chime",
	"The logo first, then the resumed game fades in",
	"Power on straight back into the game you left", "Games carry on where you left them (B: fresh)",
	"The language of the menus", "The clock the games and the menus keep", 0, 0, 0, 0, 0,
	"Hours from UTC, as the Android clock is kept", 0, 0, 0 };
static const unsigned char k_kind[PS_N] = {
	PSK_STEP, PSK_STEP, PSK_SWITCH, PSK_PICK, PSK_PICK, PSK_PAGE, PSK_PAGE, PSK_ACTION,
	PSK_SWITCH, PSK_SWITCH, PSK_SWITCH, PSK_SWITCH, PSK_SWITCH, PSK_SWITCH,
	PSK_PICK, PSK_PAGE, PSK_STEP, PSK_STEP, PSK_STEP, PSK_STEP, PSK_STEP, PSK_STEP, PSK_SWITCH,
	PSK_INFO, PSK_SWITCH };
static const char *const k_page_name[PSP_N] = { "", "BIOS boot", "Game resume", "Date & Time" };
static const signed char k_page_row[PSP_N] = { -1, PS_BIOS, PS_RESUME, PS_CLOCK };

const char *pico_set_name(int id) { return id >= 0 && id < PS_N ? k_name[id] : ""; }
const char *pico_set_about(int id) { return id >= 0 && id < PS_N ? k_about[id] : 0; }
int  pico_set_kind(int id) { return id >= 0 && id < PS_N ? k_kind[id] : PSK_INFO; }
int  pico_set_page_row(int page) { return page > 0 && page < PSP_N ? k_page_row[page] : -1; }
const char *pico_set_page_name(int page) { return page >= 0 && page < PSP_N ? k_page_name[page] : ""; }
int  pico_set_page(int id)
{
	int p;
	for (p = 1; p < PSP_N; p++) if (k_page_row[p] == id) return p;
	return PSP_NONE;
}

int pico_set_switch(int id)
{
	switch (id) {
	case PS_SKIP_INTRO:   return ayaneo_get_skip_gba_intro() ? 1 : 0;
	case PS_MUTE_BIOS:    return ayaneo_get_mute_bios() ? 1 : 0;
	case PS_MUTE_MENU:    return ayaneo_get_mute_menu() ? 1 : 0;
	case PS_INTRO_RESUME: return ayaneo_get_intro_on_resume() ? 1 : 0;
	case PS_RESUME_BOOT:  return ayaneo_get_resume_on_boot() ? 1 : 0;
	case PS_RESUME_OPEN:  return ayaneo_get_resume_on_open() ? 1 : 0;
	case PS_CLOCK12:      return pico_clock_12h() ? 0 : 1;   /* the row is "24-hour clock" */
	default:              return -1;
	}
}

void pico_set_value(int id, char *buf, int n, int *num)
{
	int sw = pico_set_switch(id);
	*num = -1;
	buf[0] = 0;
	if (sw >= 0 && id != PS_INTRO_BG) { snprintf(buf, n, "%s", tr(sw ? "On" : "Off")); return; }
	switch (id) {
	case PS_BRIGHT:   *num = ayaneo_brightness_pct(); snprintf(buf, n, "%d%%", *num); break;
	case PS_VOLUME:   *num = ayaneo_gbc_audio_get_volume(); snprintf(buf, n, "%d%%", *num); break;
	case PS_BFI:      snprintf(buf, n, "%s", tr(ayaneo_strobe_name(ayaneo_get_strobe()))); break;
	case PS_INTRO_BG: snprintf(buf, n, "%s", tr(ayaneo_get_intro_black() ? "Black" : "White")); break;
	case PS_YEAR: case PS_MONTH: case PS_DAY: case PS_HOUR: case PS_MINUTE: {
		pico_tm tm;
		pico_clock_split(pico_clock_local(), &tm);
		if (id == PS_YEAR) snprintf(buf, n, "%d", tm.year);
		else if (id == PS_MONTH) snprintf(buf, n, "%s", pico_clock_month_name(tm.mon));
		else if (id == PS_DAY) snprintf(buf, n, "%d", tm.mday);
		else if (id == PS_HOUR) snprintf(buf, n, "%02d", tm.hour);
		else snprintf(buf, n, "%02d", tm.min);
		break;
	}
	case PS_ZONE:     pico_clock_zone_text(pico_clock_zone(), buf, n); break;
	case PS_ABOUT:    snprintf(buf, n, "%s", pico_version()); break;
	default:          break;   /* a page, an action, the theme's own */
	}
}

int pico_set_change(int id, int dir, int act)
{
	int flip = dir || act;
	switch (id) {
	case PS_BRIGHT:
		if (!dir) return 0;
		ayaneo_brightness_step(dir);
		break;
	case PS_VOLUME: {
		int v = ayaneo_gbc_audio_get_volume() + dir * 5;
		if (!dir) return 0;
		ayaneo_gbc_audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
		break;
	}
	case PS_BFI:
		if (!flip) return 0;
		ayaneo_set_strobe((ayaneo_get_strobe() + (dir ? dir : 1) + 4) % 4);
		break;
	case PS_MUTE_BIOS:    if (!flip) return 0; ayaneo_set_mute_bios(!ayaneo_get_mute_bios()); break;
	case PS_MUTE_MENU:    if (!flip) return 0; ayaneo_set_mute_menu(!ayaneo_get_mute_menu()); break;
	case PS_SKIP_INTRO:   if (!flip) return 0; ayaneo_set_skip_gba_intro(!ayaneo_get_skip_gba_intro()); break;
	case PS_INTRO_BG:     if (!flip) return 0; ayaneo_set_intro_black(!ayaneo_get_intro_black()); break;
	case PS_INTRO_RESUME: if (!flip) return 0; ayaneo_set_intro_on_resume(!ayaneo_get_intro_on_resume()); break;
	case PS_RESUME_BOOT:  if (!flip) return 0; ayaneo_set_resume_on_boot(!ayaneo_get_resume_on_boot()); break;
	case PS_RESUME_OPEN:  if (!flip) return 0; ayaneo_set_resume_on_open(!ayaneo_get_resume_on_open()); break;
	case PS_CLOCK12:      if (!flip) return 0; pico_clock_set_12h(!pico_clock_12h()); break;
	case PS_YEAR: case PS_MONTH: case PS_DAY: case PS_HOUR: case PS_MINUTE: {	/* the RTC takes it at once */
		pico_tm tm;
		int days;
		if (!dir) return 0;
		pico_clock_split(pico_clock_local(), &tm);
		if (id == PS_YEAR) tm.year = tm.year + dir < 2024 ? 2095 : tm.year + dir > 2095 ? 2024 : tm.year + dir;   /* the RTC holds 1968..2095 */
		if (id == PS_MONTH) tm.mon = (tm.mon - 1 + dir + 12) % 12 + 1;
		days = pico_clock_days_in(tm.year, tm.mon);
		if (id == PS_DAY) tm.mday = (tm.mday - 1 + dir + days) % days + 1;
		if (tm.mday > days) tm.mday = days;
		if (id == PS_HOUR) tm.hour = (tm.hour + dir + 24) % 24;
		if (id == PS_MINUTE) tm.min = (tm.min + dir + 60) % 60;
		tm.sec = 0;
		pico_clock_set_local(&tm);
		return 1;   /* the clock is not a saved setting */
	}
	case PS_ZONE: {	/* quarter hours, as the world's zones go */
		int q = pico_clock_zone() + dir;
		if (!dir) return 0;
		pico_clock_set_zone(q < -48 ? 56 : q > 56 ? -48 : q);
		break;
	}
	case PS_BOOT_OS:
		if (act) ayaneo_boot_to_os();   /* never returns */
		return 0;
	default:
		return 0;
	}
	ayaneo_menu_settings_persist();
	return 1;
}
