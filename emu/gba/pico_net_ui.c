/*
 * pico_net_ui.c - the Network tab's flow (pico_net_ui.h). Only on the consoles with Wi-Fi.
 *
 * Screens: the home (Wi-Fi, Box art, RetroAchievements), Wi-Fi (on or off, the network joined,
 * the networks around strongest first, search again; A joins, X forgets a saved one), the
 * keyboard (a password, an account), Box art (the source, the region, start; then its progress),
 * the saved networks (every one kept on the card, in range or not; A twice forgets one).
 * Turning Wi-Fi on reads its firmware from the card, which takes a moment: it happens on the
 * next frame, after the screen says so.
 */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
#include <string.h>
#include <printf.h>
#include "pico_net_ui.h"
#include "pico_i18n.h"
#include "pico_scrape.h"
#include "../../platform/common/pico/pico_netsvc.h"
#include "pico_ra.h"
#include "gba_sd_save.h"

#define MAX_APS		24
#define LIST_ROWS	8		/* networks in view at once */

enum { KB_WIFI_PASS, KB_RA_USER, KB_RA_PASS };
enum { LAYOUT_LOWER, LAYOUT_UPPER, LAYOUT_SYMBOLS };

static int s_screen = NUI_CLOSED, s_sel, s_top;
static fat_vol *s_vol;
static const gba_rom_entry *s_roms;
static int s_nrom;

static struct netsvc_status s_st;
static struct netsvc_ap s_aps[MAX_APS];
static int s_naps;
static unsigned int s_saved_gen_stored = (unsigned)-1;
static int s_start_pending;		/* Wi-Fi to start on the next frame (the screen says so first) */
static char s_msg[96];			/* the last thing worth saying ("Wrong password?") */
static int s_was_joining;
#define MAX_SAVED	16		/* (the service keeps as many) */
static char s_saved[MAX_SAVED][33];	/* the saved networks' names, read when their page opens */
static int s_nsaved, s_forget_confirm = -1;	/* the row A was pressed on once: the next A forgets */
static int s_art_running, s_art_source, s_art_region;
static char s_val[NUI_ROWS][48];

/* the keyboard */
static struct {
	int kind, layout, r, c, n;
	char text[64];
	char ssid[33];
	char field[64];
	int back;			/* the screen to go back to */
} K;
static char s_ra_name[64], s_ra_pass[64];	/* typed for signing in (the password only until then) */
static int s_ra_confirm;			/* Sign out pressed once: the next A signs out */
enum { RA_ROW_ACCOUNT, RA_ROW_POINTS, RA_ROW_WAITING, RA_ROW_SIGN_OUT, RA_IN_ROWS };

static const char *const k_layouts[3][4] = {
	{ "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm.-_" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM.-_" },
	{ "1234567890", "!@#$%^&*()", "_=+[]{}\\|~", "`;:'\"<>/?," },
};
/* the bottom row: Shift (two columns), symbols (two), Space (three), Delete (one), OK (two) */
enum { WIDE_SHIFT, WIDE_SYMBOLS, WIDE_SPACE, WIDE_DEL, WIDE_OK };
static const int k_wide_col[5] = { 0, 2, 4, 7, 8 }, k_wide_span[5] = { 2, 2, 3, 1, 2 };
static int wide_at(int c) { int i; for (i = 4; i > 0; i--) if (c >= k_wide_col[i]) return i; return 0; }

static const char *const k_regions[4] = { "wor", "us", "eu", "jp" };
static const char *const k_region_names[4] = { "World", "USA", "Europe", "Japan" };

/* Wi-Fi left on when the console was last used: /gammaos/net/wifi.on holds '1' (on) or '0'. Read
 * when the card arrives; written by the Wi-Fi row on the menu's thread */
static void wifi_on_store(int on)
{
	unsigned char b = on ? '1' : '0';
	if (s_vol) gba_sd_write_named(s_vol, "/gammaos/net", "wifi", "on", &b, 1);
}
static int wifi_on_load(fat_vol *vol)
{
	unsigned char b = 0;
	return gba_sd_read_named(vol, "/gammaos/net", "wifi", "on", &b, 1) == 1 && b == '1';
}

void nui_set_roms(fat_vol *vol, const gba_rom_entry *roms, int nrom)
{
	static int resumed;
	if (vol != s_vol && vol) netsvc_saved_load(vol);	/* the card's saved networks, once per card */
	/* Wi-Fi on at the last use: on again (once per power on), two seconds after the menu is up
	 * so its start never holds the menu's first frames; a saved network in range is joined */
	if (vol && !resumed) {
		struct netsvc_status ns;
		resumed = 1;
		netsvc_get(&ns);
		if (ns.state == NETSVC_OFF && wifi_on_load(vol)) s_start_pending = 120;
	}
	pico_ra_card(vol);					/* and RetroAchievements' account */
	s_vol = vol;
	s_roms = roms;
	s_nrom = nrom;
}

void nui_open(void)
{
	s_screen = NUI_HOME;
	s_sel = 0;
	s_msg[0] = 0;
}

int nui_is_open(void) { return s_screen != NUI_CLOSED; }
void nui_close(void) { s_screen = NUI_CLOSED; }

const char *nui_wifi_summary(void)
{
	static char b[48];
	switch (s_st.state) {
	case NETSVC_OFF:      return tr("Off");
	case NETSVC_STARTING: return tr("Starting...");
	case NETSVC_FAILED:   return tr("Not working");
	case NETSVC_SCANNING: return tr("Searching...");
	case NETSVC_JOINING:  return tr("Connecting...");
	case NETSVC_ONLINE:   snprintf(b, sizeof b, "%s", s_st.ssid); return b;
	default:              return tr("Not connected");
	}
}

/* the games the menu found no box art for (its art index), -1 not known */
static int art_missing(void)
{
	extern int gba_menu_art_missing(void);
	return gba_menu_art_missing();
}

/* ---- the Wi-Fi screen's rows: Wi-Fi on/off, then (when on) the networks and Search again, then
 * the saved networks (also with Wi-Fi off: forgetting needs no radio) ---- */
enum { ROW_TOGGLE, ROW_NET, ROW_SEARCH, ROW_SAVED };
static int wifi_rows(int *kind, int *ap, int max)
{
	int n = 0, i;
	kind[n] = ROW_TOGGLE; ap[n++] = -1;
	if (s_st.state != NETSVC_OFF && s_st.state != NETSVC_FAILED && s_st.state != NETSVC_STARTING) {
		for (i = 0; i < s_naps && n < max - 2; i++) { kind[n] = ROW_NET; ap[n++] = i; }
		kind[n] = ROW_SEARCH; ap[n++] = -1;
	}
	kind[n] = ROW_SAVED; ap[n++] = -1;
	return n;
}

static void saved_read(void) { s_nsaved = netsvc_saved_names(s_saved, MAX_SAVED); }

/* how a saved network stands now: connected, being joined, in the last search, or "" */
static const char *saved_state(const char *ssid)
{
	int i;
	if (s_st.state == NETSVC_ONLINE && !strcmp(s_st.ssid, ssid)) return tr("Connected");
	if (s_st.state == NETSVC_JOINING && !strcmp(s_st.ssid, ssid)) return tr("Connecting...");
	if (s_st.state != NETSVC_OFF)
		for (i = 0; i < s_naps; i++)
			if (!strcmp(s_aps[i].ssid, ssid)) return tr("In range");
	return "";
}

static const char *signal_word(int dbm)
{
	return dbm >= -60 ? tr("Strong") : dbm >= -72 ? tr("Good") : tr("Weak");
}

/* ---- the keyboard ---- */
static void kb_open(int kind, const char *ssid)
{
	memset(&K, 0, sizeof K);
	K.kind = kind;
	K.back = s_screen;
	if (kind == KB_WIFI_PASS) {
		strncpy(K.ssid, ssid, sizeof K.ssid - 1);
		snprintf(K.field, sizeof K.field, tr("Password for %s"), ssid);
	} else {
		snprintf(K.field, sizeof K.field, "%s", kind == KB_RA_USER ? tr("RetroAchievements user name") :
			 tr("RetroAchievements password"));
		if (kind == KB_RA_USER) {		/* the name typed before, to change */
			strncpy(K.text, s_ra_name, sizeof K.text - 1);
			K.n = (int)strlen(K.text);
		}
	}
	K.r = 1;
	s_screen = NUI_KEYBOARD;
}

static void kb_done(void)
{
	if (K.kind == KB_WIFI_PASS) {
		if (K.n < 8) { snprintf(s_msg, sizeof s_msg, "%s", tr("A Wi-Fi password has 8 to 63 characters")); return; }
		netsvc_join(K.ssid, K.text);
		s_was_joining = 1;
		s_msg[0] = 0;
	} else if (K.kind == KB_RA_USER) {
		memcpy(s_ra_name, K.text, sizeof s_ra_name);
	} else if (K.kind == KB_RA_PASS) {
		memcpy(s_ra_pass, K.text, sizeof s_ra_pass);
	}
	memset(K.text, 0, sizeof K.text);
	K.n = 0;
	s_screen = K.back;
}

static void kb_press(void)
{
	if (K.r < 4) {
		if (K.n < 63) { K.text[K.n++] = k_layouts[K.layout][K.r][K.c]; K.text[K.n] = 0; }
		if (K.layout == LAYOUT_UPPER) K.layout = LAYOUT_LOWER;	/* a capital, then small letters again */
		return;
	}
	switch (wide_at(K.c)) {
	case WIDE_SHIFT:   K.layout = K.layout == LAYOUT_LOWER ? LAYOUT_UPPER : LAYOUT_LOWER; break;
	case WIDE_SYMBOLS: K.layout = K.layout == LAYOUT_SYMBOLS ? LAYOUT_LOWER : LAYOUT_SYMBOLS; break;
	case WIDE_SPACE:   if (K.n < 63) { K.text[K.n++] = ' '; K.text[K.n] = 0; } break;
	case WIDE_DEL:     if (K.n) K.text[--K.n] = 0; break;
	case WIDE_OK:      kb_done(); break;
	}
}

static int kb_input(int e)
{
	if (e & NUI_B) {
		if (K.n) { K.text[--K.n] = 0; return NUI_ACT_NONE; }	/* B deletes, then leaves */
		s_screen = K.back;
		memset(&K, 0, sizeof K);
		return NUI_ACT_NONE;
	}
	if (e & NUI_Y) { K.layout = K.layout == LAYOUT_UPPER ? LAYOUT_LOWER : LAYOUT_UPPER; return NUI_ACT_NONE; }
	if (e & NUI_X) { kb_done(); return NUI_ACT_NONE; }
	if (e & NUI_UP) { if (K.r > 0) K.r--; if (K.r == 3 && K.c > 9) K.c = 9; }
	if (e & NUI_DOWN) { if (K.r < 4) K.r++; if (K.r == 4) K.c = k_wide_col[wide_at(K.c)]; }
	if (e & NUI_LEFT) {
		if (K.r == 4) { int w = wide_at(K.c); K.c = k_wide_col[w > 0 ? w - 1 : 4]; }
		else K.c = K.c > 0 ? K.c - 1 : 9;
	}
	if (e & NUI_RIGHT) {
		if (K.r == 4) { int w = wide_at(K.c); K.c = k_wide_col[w < 4 ? w + 1 : 0]; }
		else K.c = K.c < 9 ? K.c + 1 : 0;
	}
	if (e & NUI_A) kb_press();
	return NUI_ACT_NONE;
}

/* ---- input ---- */
int nui_input(int e)
{
	int kind[NUI_ROWS + MAX_APS], ap[NUI_ROWS + MAX_APS], n, i;
	switch (s_screen) {
	case NUI_HOME:
		if (e & NUI_UP) s_sel = s_sel > 0 ? s_sel - 1 : 2;
		if (e & NUI_DOWN) s_sel = s_sel < 2 ? s_sel + 1 : 0;
		if (e & NUI_B) { s_screen = NUI_CLOSED; return NUI_ACT_CLOSE; }
		if (e & NUI_A) {
			s_screen = s_sel == 0 ? NUI_WIFI : s_sel == 1 ? NUI_ART : NUI_RA;
			s_sel = 0; s_top = 0;
			s_msg[0] = 0;
		}
		return NUI_ACT_NONE;
	case NUI_RA: {
		int in = pico_ra_state() == PICO_RA_SIGNED_IN, rows = in ? RA_IN_ROWS : 3;
		if (e & NUI_B) {
			if (s_ra_confirm) { s_ra_confirm = 0; return NUI_ACT_NONE; }	/* B: not signing out after all */
			s_screen = NUI_HOME; s_sel = 2; memset(s_ra_pass, 0, sizeof s_ra_pass);
			return NUI_ACT_NONE;
		}
		if (e & (NUI_UP | NUI_DOWN)) s_ra_confirm = 0;
		if (e & NUI_UP) s_sel = s_sel > 0 ? s_sel - 1 : rows - 1;
		if (e & NUI_DOWN) s_sel = s_sel < rows - 1 ? s_sel + 1 : 0;
		if (s_sel >= rows) s_sel = rows - 1;
		if (!(e & NUI_A)) return NUI_ACT_NONE;
		s_msg[0] = 0;
		if (in) {
			if (s_sel == RA_ROW_WAITING && pico_ra_waiting()) {
				if (s_st.state != NETSVC_ONLINE) snprintf(s_msg, sizeof s_msg, "%s", tr("Connect to a Wi-Fi network first"));
				else { pico_ra_send_now(); snprintf(s_msg, sizeof s_msg, "%s", tr("Sending the unlocks...")); }
			} else if (s_sel == RA_ROW_SIGN_OUT) {
				if (!s_ra_confirm) s_ra_confirm = 1;	/* once more to be sure */
				else { s_ra_confirm = 0; pico_ra_sign_out(); s_sel = 0; }
			}
		} else if (s_sel == 0) {
			kb_open(KB_RA_USER, "");
		} else if (s_sel == 1) {
			kb_open(KB_RA_PASS, "");
		} else if (pico_ra_state() != PICO_RA_SIGNING_IN) {
			if (s_st.state != NETSVC_ONLINE) snprintf(s_msg, sizeof s_msg, "%s", tr("Connect to a Wi-Fi network first"));
			else if (!s_ra_name[0] || !s_ra_pass[0]) snprintf(s_msg, sizeof s_msg, "%s", tr("Type your user name and password"));
			else {
				pico_ra_sign_in(s_ra_name, s_ra_pass);
				memset(s_ra_pass, 0, sizeof s_ra_pass);
			}
		}
		return NUI_ACT_NONE;
	}
	case NUI_WIFI:
		n = wifi_rows(kind, ap, NUI_ROWS + MAX_APS);
		if (e & NUI_B) { s_screen = NUI_HOME; s_sel = 0; return NUI_ACT_NONE; }
		if (e & NUI_UP) s_sel = s_sel > 0 ? s_sel - 1 : n - 1;
		if (e & NUI_DOWN) s_sel = s_sel < n - 1 ? s_sel + 1 : 0;
		if (s_sel >= n) s_sel = n - 1;
		if (s_sel < s_top) s_top = s_sel;
		if (s_sel >= s_top + LIST_ROWS) s_top = s_sel - LIST_ROWS + 1;
		if ((e & NUI_X) && kind[s_sel] == ROW_NET && s_aps[ap[s_sel]].saved) {
			netsvc_forget(s_aps[ap[s_sel]].ssid);
			if (s_st.state == NETSVC_ONLINE && !strcmp(s_st.ssid, s_aps[ap[s_sel]].ssid)) netsvc_leave();
			snprintf(s_msg, sizeof s_msg, tr("%s forgotten"), s_aps[ap[s_sel]].ssid);
			s_aps[ap[s_sel]].saved = 0;
		}
		if (!(e & NUI_A)) return NUI_ACT_NONE;
		s_msg[0] = 0;
		if (kind[s_sel] == ROW_TOGGLE) {
			if (s_st.state == NETSVC_OFF || s_st.state == NETSVC_FAILED) { s_start_pending = 2; wifi_on_store(1); }
			else { netsvc_wifi(0); s_naps = 0; wifi_on_store(0); }
		} else if (kind[s_sel] == ROW_SEARCH) {
			netsvc_scan();
		} else if (kind[s_sel] == ROW_SAVED) {
			saved_read();
			s_screen = NUI_SAVED;
			s_sel = 0; s_top = 0;
			s_forget_confirm = -1;
		} else {
			const struct netsvc_ap *a = &s_aps[ap[s_sel]];
			if (s_st.state == NETSVC_ONLINE && !strcmp(s_st.ssid, a->ssid)) {
				netsvc_leave();			/* A on the network joined: leave it */
			} else if (!a->usable) {
				snprintf(s_msg, sizeof s_msg, "%s", tr("This network's security is not supported yet"));
			} else if (a->saved || !a->secured) {
				netsvc_join(a->ssid, a->saved ? 0 : "");
				s_was_joining = 1;
			} else {
				kb_open(KB_WIFI_PASS, a->ssid);
			}
		}
		return NUI_ACT_NONE;
	case NUI_KEYBOARD:
		return kb_input(e);
	case NUI_SAVED:
		if (e & NUI_B) {
			if (s_forget_confirm >= 0) { s_forget_confirm = -1; return NUI_ACT_NONE; }	/* B: keep it after all */
			s_screen = NUI_WIFI; s_top = 0;
			n = wifi_rows(kind, ap, NUI_ROWS + MAX_APS);
			s_sel = n - 1;			/* back on the Saved networks row */
			if (s_sel >= LIST_ROWS) s_top = s_sel - LIST_ROWS + 1;
			s_msg[0] = 0;
			return NUI_ACT_NONE;
		}
		if (!s_nsaved) return NUI_ACT_NONE;
		if (e & (NUI_UP | NUI_DOWN)) s_forget_confirm = -1;
		if (e & NUI_UP) s_sel = s_sel > 0 ? s_sel - 1 : s_nsaved - 1;
		if (e & NUI_DOWN) s_sel = s_sel < s_nsaved - 1 ? s_sel + 1 : 0;
		if (s_sel >= s_nsaved) s_sel = s_nsaved - 1;
		if (s_sel < s_top) s_top = s_sel;
		if (s_sel >= s_top + LIST_ROWS) s_top = s_sel - LIST_ROWS + 1;
		if (e & (NUI_A | NUI_X)) {
			if (s_forget_confirm != s_sel) { s_forget_confirm = s_sel; s_msg[0] = 0; return NUI_ACT_NONE; }
			netsvc_forget(s_saved[s_sel]);	/* (nui_tick writes the card's list) */
			if ((s_st.state == NETSVC_ONLINE || s_st.state == NETSVC_JOINING) && !strcmp(s_st.ssid, s_saved[s_sel]))
				netsvc_leave();
			snprintf(s_msg, sizeof s_msg, tr("%s forgotten"), s_saved[s_sel]);
			for (i = 0; i < s_naps; i++)
				if (!strcmp(s_aps[i].ssid, s_saved[s_sel])) s_aps[i].saved = 0;
			s_forget_confirm = -1;
			saved_read();
			if (s_sel >= s_nsaved) s_sel = s_nsaved ? s_nsaved - 1 : 0;
			if (s_top > s_sel) s_top = s_sel;
		}
		return NUI_ACT_NONE;
	case NUI_ART: {
		struct pico_scrape_info si;
		pico_scrape_get(&si);
		if (si.running) {
			if (e & NUI_B) pico_scrape_cancel();
			return NUI_ACT_NONE;
		}
		if (e & NUI_B) { s_screen = NUI_HOME; s_sel = 1; return NUI_ACT_NONE; }
		if (e & NUI_UP) s_sel = s_sel > 0 ? s_sel - 1 : 2;
		if (e & NUI_DOWN) s_sel = s_sel < 2 ? s_sel + 1 : 0;
		if (e & (NUI_A | NUI_LEFT | NUI_RIGHT)) {
			if (s_sel == 1 && pico_scrape_has_screenscraper()) s_art_source ^= 1;
			if (s_sel == 2) s_art_region = (s_art_region + ((e & NUI_LEFT) ? 3 : 1)) % 4;
			if (s_sel == 0 && (e & NUI_A)) {
				if (s_st.state != NETSVC_ONLINE) {
					snprintf(s_msg, sizeof s_msg, "%s", tr("Connect to a Wi-Fi network first"));
				} else if (s_vol && s_roms) {
					pico_scrape_start(s_vol, s_roms, s_nrom,
							  pico_scrape_has_screenscraper() && !s_art_source ? SCRAPE_BOTH : SCRAPE_LIBRETRO,
							  k_regions[s_art_region]);
					s_art_running = 1;
					s_msg[0] = 0;
				}
			}
		}
		return NUI_ACT_NONE;
	}
	default:
		return NUI_ACT_NONE;
	}
	(void)i;
}

/* ---- every frame ---- */
volatile int g_dbg_nui_scrape;	/* oem netsvc:scrape: 1 start, 2 stop */
volatile int g_dbg_nui_art;	/* oem netsvc:art - the menu acts as if box art had arrived (a test) */
int nui_tick(void)
{
	int act = NUI_ACT_NONE;
	if (g_dbg_nui_art) { g_dbg_nui_art = 0; act = NUI_ACT_ART; }
	if (g_dbg_nui_scrape) {	/* oem netsvc:scrape[:stop] - box art as the page's Find row / B does */
		struct pico_scrape_info si;
		pico_scrape_get(&si);
		if (g_dbg_nui_scrape == 2 && si.running) pico_scrape_cancel();
		else if ((g_dbg_nui_scrape == 1 || g_dbg_nui_scrape == 3) && !si.running && s_vol && s_roms)
			pico_scrape_start(s_vol, s_roms, s_nrom, pico_scrape_has_screenscraper() && g_dbg_nui_scrape == 1 ?
					  SCRAPE_BOTH : SCRAPE_LIBRETRO,
					  k_regions[s_art_region]), s_art_running = 1;
		g_dbg_nui_scrape = 0;
	}
	if (s_start_pending && --s_start_pending == 0) netsvc_wifi(1);	/* the screen said "Starting" first */
	netsvc_get(&s_st);
	if (s_start_pending) s_st.state = NETSVC_STARTING;
	if (s_st.state != NETSVC_OFF) s_naps = netsvc_aps(s_aps, MAX_APS);
	if (s_st.saved_gen != s_saved_gen_stored && s_vol) {	/* a network joined or forgotten: on the card */
		if (s_saved_gen_stored != (unsigned)-1) netsvc_saved_store(s_vol);
		s_saved_gen_stored = s_st.saved_gen;
	}
	netsvc_clock_apply();
	{	/* the time for the service's certificate checks, read here on the menu thread */
		static unsigned int frames;
		if (frames++ % 300 == 0) netsvc_time_hint();
	}
	if (s_was_joining && s_st.state != NETSVC_JOINING) {	/* how the join went */
		s_was_joining = 0;
		if (s_st.state == NETSVC_ONLINE) snprintf(s_msg, sizeof s_msg, tr("Connected to %s"), s_st.ssid);
		else if (s_st.why && *s_st.why) snprintf(s_msg, sizeof s_msg, "%s", s_st.why);
	}
	if (s_art_running && !pico_scrape_step()) {
		struct pico_scrape_info si;
		s_art_running = 0;
		pico_scrape_get(&si);
		snprintf(s_msg, sizeof s_msg, tr("Box art: %d found, %d not found"), si.found, si.missing);
		if (si.found) {
			extern void gba_menu_art_forget(void);
			gba_menu_art_forget();	/* the same games, new art: the menu reads it afresh */
			act = NUI_ACT_ART;
		}
	}
	return act;
}

/* ---- what to draw ---- */
void nui_view(struct nui_view *v)
{
	int kind[NUI_ROWS + MAX_APS], ap[NUI_ROWS + MAX_APS], n, i;
	memset(v, 0, sizeof *v);
	v->screen = s_screen;
	v->sel = -1;
	switch (s_screen) {
	case NUI_HOME:
		v->title = tr("Network");
		v->nrows = 3;
		v->rows[0] = tr("Wi-Fi");
		v->values[0] = nui_wifi_summary();
		v->rows[1] = tr("Box art");
		if (s_art_running) v->values[1] = tr("Working...");
		else if (art_missing() > 0) {
			snprintf(s_val[1], sizeof s_val[1], tr("%d without art"), art_missing());
			v->values[1] = s_val[1];
		} else v->values[1] = art_missing() == 0 ? tr("Every game has art") : tr("From the network");
		v->rows[2] = "RetroAchievements";
		v->values[2] = pico_ra_state() == PICO_RA_SIGNED_IN ? pico_ra_user() :
			       pico_ra_state() == PICO_RA_SIGNING_IN ? tr("Signing in...") : tr("Not signed in");
		v->sel = s_sel;
		if (s_st.state == NETSVC_ONLINE) {
			snprintf(s_val[0], sizeof s_val[0], "%u.%u.%u.%u", s_st.ip >> 24, (s_st.ip >> 16) & 255, (s_st.ip >> 8) & 255,
				 s_st.ip & 255);
			v->status = s_val[0];
		}
		v->nhints = 2;
		v->hint_code[0] = "A"; v->hint_label[0] = tr("Select");
		v->hint_code[1] = "B"; v->hint_label[1] = tr("Back");
		break;
	case NUI_WIFI:
		v->title = tr("Wi-Fi");
		n = wifi_rows(kind, ap, NUI_ROWS + MAX_APS);
		if (s_sel >= n) s_sel = n - 1;
		for (i = s_top; i < n && v->nrows < LIST_ROWS; i++) {
			int k = v->nrows;
			if (kind[i] == ROW_TOGGLE) {
				v->rows[k] = tr("Wi-Fi");
				v->values[k] = s_st.state == NETSVC_OFF ? tr("Off") : s_st.state == NETSVC_STARTING ? tr("Starting...") :
					       s_st.state == NETSVC_FAILED ? tr("Not working") : tr("On");
			} else if (kind[i] == ROW_SEARCH) {
				v->rows[k] = s_st.state == NETSVC_SCANNING ? tr("Searching...") : tr("Search again");
			} else if (kind[i] == ROW_SAVED) {
				saved_read();			/* (the count: a join or a forget may have changed it) */
				v->rows[k] = tr("Saved networks");
				if (s_nsaved) snprintf(s_val[k], sizeof s_val[k], "%d", s_nsaved);
				v->values[k] = s_nsaved ? s_val[k] : tr("None");
			} else {
				const struct netsvc_ap *a = &s_aps[ap[i]];
				int joined = s_st.state == NETSVC_ONLINE && !strcmp(s_st.ssid, a->ssid);
				int joining = s_st.state == NETSVC_JOINING && !strcmp(s_st.ssid, a->ssid);
				v->rows[k] = a->ssid;
				snprintf(s_val[k], sizeof s_val[k], "%s%s%s", joined ? tr("Connected") : joining ? tr("Connecting...") :
					 a->saved ? tr("Saved") : !a->usable ? tr("Not supported") : a->secured ? tr("Locked") : tr("Open"),
					 "  ", signal_word(a->dbm));
				v->values[k] = s_val[k];
			}
			if (i == s_sel) v->sel = k;
			v->nrows++;
		}
		v->more_up = s_top > 0;
		v->more_down = i < n;
		v->busy = s_st.state == NETSVC_STARTING || s_st.state == NETSVC_SCANNING || s_st.state == NETSVC_JOINING;
		if (s_msg[0]) v->status = s_msg;
		else if (s_st.state == NETSVC_OFF) v->status = tr("Wi-Fi is off: it uses no power until you turn it on here.");
		else if (s_st.state == NETSVC_FAILED) v->status = s_st.why;
		else if (s_st.state == NETSVC_ONLINE) {
			snprintf(s_val[NUI_ROWS - 1], sizeof s_val[0], tr("Connected to %s"), s_st.ssid);
			v->status = s_val[NUI_ROWS - 1];
		}
		v->nhints = 3;
		v->hint_code[0] = "A"; v->hint_label[0] = tr("Select");
		v->hint_code[1] = "X"; v->hint_label[1] = tr("Forget");
		v->hint_code[2] = "B"; v->hint_label[2] = tr("Back");
		break;
	case NUI_SAVED:
		v->title = tr("Saved networks");
		if (s_sel >= s_nsaved) s_sel = s_nsaved ? s_nsaved - 1 : 0;
		for (i = s_top; i < s_nsaved && v->nrows < LIST_ROWS; i++) {
			int k = v->nrows;
			v->rows[k] = s_saved[i];
			v->values[k] = i == s_forget_confirm ? tr("Press A again to forget") : saved_state(s_saved[i]);
			if (i == s_sel) v->sel = k;
			v->nrows++;
		}
		v->more_up = s_top > 0;
		v->more_down = i < s_nsaved;
		v->status = s_msg[0] ? s_msg : !s_nsaved ? tr("No saved networks: a network is saved when you join it.") :
			    tr("Saved networks are joined by themselves when they are in range.");
		v->nhints = s_nsaved ? 2 : 1;
		if (s_nsaved) { v->hint_code[0] = "A"; v->hint_label[0] = tr("Forget"); }
		v->hint_code[v->nhints - 1] = "B"; v->hint_label[v->nhints - 1] = tr("Back");
		break;
	case NUI_RA: {
		int st = pico_ra_state();
		v->title = "RetroAchievements";
		if (st == PICO_RA_SIGNED_IN) {
			v->nrows = RA_IN_ROWS;
			v->rows[RA_ROW_ACCOUNT] = tr("Account");
			v->values[RA_ROW_ACCOUNT] = pico_ra_user();
			v->rows[RA_ROW_POINTS] = tr("Points");
			snprintf(s_val[RA_ROW_POINTS], sizeof s_val[0], "%u", pico_ra_points());
			v->values[RA_ROW_POINTS] = s_val[RA_ROW_POINTS];
			v->rows[RA_ROW_WAITING] = tr("Unlocks waiting to be sent");
			if (pico_ra_waiting()) snprintf(s_val[RA_ROW_WAITING], sizeof s_val[0], "%u", pico_ra_waiting());
			v->values[RA_ROW_WAITING] = pico_ra_waiting() ? s_val[RA_ROW_WAITING] : tr("None");
			v->rows[RA_ROW_SIGN_OUT] = s_ra_confirm ? tr("Press A again to sign out") : tr("Sign out");
		} else {
			v->nrows = 3;
			v->rows[0] = tr("User name");
			v->values[0] = s_ra_name[0] ? s_ra_name : tr("Not set");
			v->rows[1] = tr("Password");
			v->values[1] = s_ra_pass[0] ? "********" : tr("Not set");
			v->rows[2] = st == PICO_RA_SIGNING_IN ? tr("Signing in...") : tr("Sign in");
			v->busy = st == PICO_RA_SIGNING_IN;
		}
		v->sel = s_sel;
		v->status = s_msg[0] ? s_msg : st == PICO_RA_NO_BLOB ? tr("RetroAchievements is missing from the card (/cores)") :
			    st == PICO_RA_SIGNED_IN && s_sel == RA_ROW_WAITING && pico_ra_waiting() ?
				    tr("A sends them now; they also go by themselves whenever there is a network") :
			    st == PICO_RA_SIGNED_IN ? tr("Softcore. Unlocks made without a network are sent later.") :
			    pico_ra_note()[0] ? pico_ra_note() : tr("Sign in with your retroachievements.org account");
		v->nhints = 2;
		v->hint_code[0] = "A"; v->hint_label[0] = tr("Select");
		v->hint_code[1] = "B"; v->hint_label[1] = tr("Back");
		break;
	}
	case NUI_KEYBOARD: {
		struct nui_kb *k = &v->kb;
		int secret = K.kind != KB_RA_USER;
		v->title = K.kind == KB_WIFI_PASS ? tr("Wi-Fi") : "RetroAchievements";
		k->field = K.field;
		for (i = 0; i < K.n && i < (int)sizeof k->shown - 1; i++) k->shown[i] = secret ? '*' : K.text[i];	/* a password: dots */
		k->shown[i] = 0;
		if (K.n && secret) k->shown[K.n - 1] = K.text[K.n - 1];	/* the last character typed shows */
		for (i = 0; i < 4; i++) k->keys[i] = k_layouts[K.layout][i];
		for (i = 0; i < 5; i++) {
			static const char *const labels[5] = { "Shift", "#+=", "Space", "Del", "OK" };
			k->wide[i].label = i == WIDE_SYMBOLS && K.layout == LAYOUT_SYMBOLS ? "abc" : tr(labels[i]);
			k->wide[i].col = k_wide_col[i];
			k->wide[i].span = k_wide_span[i];
		}
		k->r = K.r;
		k->c = K.r == 4 ? k_wide_col[wide_at(K.c)] : K.c;
		k->note = s_msg[0] ? s_msg : K.kind == KB_WIFI_PASS ? tr("8 to 63 characters") : 0;
		v->nhints = 4;
		v->hint_code[0] = "A"; v->hint_label[0] = tr("Type");
		v->hint_code[1] = "Y"; v->hint_label[1] = tr("Shift");
		v->hint_code[2] = "X"; v->hint_label[2] = tr("Done");
		v->hint_code[3] = "B"; v->hint_label[3] = tr("Delete");
		break;
	}
	case NUI_ART: {
		struct pico_scrape_info si;
		pico_scrape_get(&si);
		v->title = tr("Box art");
		if (si.running) {
			v->subtitle = si.game;
			snprintf(s_val[0], sizeof s_val[0], tr("%d of %d games: %d found"), si.done, si.total, si.found);
			v->status = s_val[0];
			v->busy = 1;
			v->done = (unsigned)si.done;
			v->total = si.total ? (unsigned)si.total : 1;
			v->nhints = 1;
			v->hint_code[0] = "B"; v->hint_label[0] = tr("Stop");
			break;
		}
		v->nrows = 3;
		if (art_missing() > 0) {
			snprintf(s_val[0], sizeof s_val[0], art_missing() == 1 ? tr("Find box art for %d game") :
				 tr("Find box art for %d games"), art_missing());
			v->rows[0] = s_val[0];
		} else v->rows[0] = art_missing() == 0 ? tr("Look again for box art") : tr("Find box art for games without it");
		v->rows[1] = tr("Source");
		v->values[1] = pico_scrape_has_screenscraper() && !s_art_source ? "libretro, then ScreenScraper" : "libretro thumbnails";
		v->rows[2] = tr("Region first");
		v->values[2] = tr(k_region_names[s_art_region]);
		v->sel = s_sel;
		v->status = s_msg[0] ? s_msg : s_st.state != NETSVC_ONLINE ? tr("Connect to a Wi-Fi network first") : 0;
		v->nhints = 2;
		v->hint_code[0] = "A"; v->hint_label[0] = tr("Select");
		v->hint_code[1] = "B"; v->hint_label[1] = tr("Back");
		break;
	}
	default:
		break;
	}
}
#endif
