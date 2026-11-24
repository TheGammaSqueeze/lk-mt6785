/*
 * pico_link_ui.c - playing together over Bluetooth, the flow every menu theme shares
 * (pico_link_ui.h). Themes draw lui_view() their own way; the steps and the words are here.
 *
 * Play together (host): Bluetooth on, this console advertises that it hosts a game of its system
 * and waits. A guest that opens a link gets 'I' (the game: system, size, name) and answers 'J'
 * (0 it plays, 1 it cannot), then both launch, armed for the session (pico_np_arm), and the
 * session's own start (pico_netplay.c) does the rest.
 *
 * Join (guest): the hosts nearby, picked with A; once the host's game is known, a link cable
 * system (GB, GBC, GBA) lets the guest pick its own game of that system (the theme's library,
 * LUI_ACT_PICK); the others play the host's game, which must be on this card too.
 *
 * Send: the consoles nearby with their Receive screen open, picked with A; the game's file goes,
 * then its save and its clock, each landing at the same place on the other card (pico_xfer.c),
 * and 'E' says that was all. A file the other card has already is kept there, never overwritten
 * (a friend's save must not replace the other console's own progress).
 *
 * Receive: this console advertises it is ready and takes each file as it comes, until the sender
 * says that was all; the theme then lists the card again (LUI_ACT_RELIST).
 *
 * Bluetooth is on only while one of these screens or a session needs it.
 */
#include <string.h>
#include <printf.h>
#include "pico_link_ui.h"
#include "pico_i18n.h"
#include "../../platform/common/pico/pico_link.h"
#include "../../platform/common/pico/pico_netplay.h"
#include "../../platform/common/pico/pico_xfer.h"
#include "fat_ro.h"
#include "gba_sd_save.h"
#include "rom_archive.h"          /* RA_OK: a game's identity read */
#include "pico_clock.h"           /* a clock that lost its time takes the other console's */

/* The words: in the menus' language, or English for a theme whose fonts lack that language's
 * letters (lui_set_english) */
static int s_english;
static const char *lt(const char *en) { return s_english ? en : tr(en); }
static const char *lt_n(int n, const char *one, const char *other)
{
	return s_english ? (n == 1 ? one : other) : tr_n(n, one, other);
}
void lui_set_english(int on) { s_english = on; }

extern int  pico_link_send(const void *p, unsigned int n);
extern int  pico_link_recv(void *buf, unsigned int cap);
extern void pico_np_arm(int host, unsigned int delay);
extern unsigned int gpt4_get_current_tick(void);

#define LUI_DELAY	2		/* the session's input delay, frames */
#if defined(KONKR_KPA)
#define PICO_BT_MODEL_NO	1	/* this console, as the others name it (lui_model_name) */
#else
#define PICO_BT_MODEL_NO	2
#endif

static const gba_rom_entry *s_roms;
static int s_nrom;
static int s_screen = LUI_CLOSED, s_sel, s_rom = -1, s_act_rom = -1, s_act_host, s_act_system;
static int s_same_rom = -1;		/* LUI_JOIN_CHOOSE: the host's game on this card */
static int s_home_rom = -1;		/* Home's Play together row: that game (-1: no such row) */
static int s_info_sent, s_bt_on;
static int s_time_sent;			/* this link has had our time ('T') */
static struct pico_peer s_peer[LUI_ROWS];
static int s_npeer;
static char s_line[160], s_sub[96], s_rowbuf[LUI_ROWS][48], s_valbuf[LUI_ROWS][64];
/* the start screen: the theme's painter, the names it shows */
static int (*s_paint)(const struct lui_progress *p);
static char s_game_mine[128], s_game_theirs[128];
static int s_other_model;
/* the host's game, as its 'I' said */
static int s_host_system = -1;
static unsigned int s_host_size;
static char s_host_name[128];
/* a lockstep game's identity, its ROM's size and CRC-32 (gba_rom_identity): sent after the name in
 * 'I' (an older console reads the name and stops at its end), so the same game is found zipped or
 * plain, under another name, and a different version under the same name is told apart */
static int s_my_id_ok, s_host_id_ok;
static uint32_t s_my_usize, s_my_crc, s_host_usize, s_host_crc;

const char *lui_model_name(int model)
{
	return model == 1 ? "Konkr Pocket Advance" : model == 2 ? "Konkr Pocket Block" : "GammaOS Pico";
}

const char *lui_system_name(int type)
{
	static const char *const n[] = { "Game Boy", "Game Boy Color", "Game Boy Advance", "Super Nintendo",
					 "Genesis", "Master System", "Game Gear", "SG-1000", "NES" };
	return type >= 0 && type <= 8 ? n[type] : "";
}

/* the link cable systems: each player runs their own cartridge */
static int link_system(int type) { return type <= 2; }

static void bt_on(void)
{
	if (!s_bt_on) {
		pico_bt_service_start();
		s_bt_on = 1;
	}
}
static void bt_off(void)
{
	if (s_bt_on) {
		pico_link_want(0);
		pico_link_offer(PICO_OFFER_NONE, 0);
		pico_bt_service_stop();
		s_bt_on = 0;
	}
}

static void put32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned int get32(const unsigned char *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static void message(const char *title, const char *why)
{
	bt_off();
	s_screen = LUI_MESSAGE;
	snprintf(s_sub, sizeof s_sub, "%s", title);
	snprintf(s_line, sizeof s_line, "%s", why);
}

int lui_open(void) { return s_screen != LUI_CLOSED; }

void lui_set_roms(const gba_rom_entry *roms, int nrom) { s_roms = roms; s_nrom = nrom; }

/* No game is copied by itself: a console without the other's game is told to copy it with Send
 * and Receive, which open on the notice about copying only games one owns (LUI_TERMS). */
static int s_terms_send;		/* the notice is in front of: 1 Send, 0 Receive */
static void receive_start(void);

/* the guest's answer to the host's game ('J'): it plays, it cannot (the game is not on its card),
 * or it left (backed out of picking, closed the screen): the host then waits for another */
enum { JOIN_PLAYS, JOIN_CANNOT, JOIN_LEFT,
       JOIN_NEED,			/* (older consoles: asked the host for its file; not sent now) */
       JOIN_OTHER };			/* this card has another version of it under that name */
static void join_reply(int answer, int rom);

void lui_close(void)
{
	if (s_screen == LUI_JOIN_WAIT)
		join_reply(JOIN_LEFT, -1);	/* the host is told: nobody plays */
	if (pico_xfer_busy())
		pico_xfer_cancel();		/* a file half sent or received is never written */
	bt_off();
	s_screen = LUI_CLOSED;
}

void lui_menu_enter(void)
{
	extern int pico_np_active(void);
	if (s_screen != LUI_CLOSED || pico_np_active())
		return;
	s_bt_on = 1;				/* whoever left it on: off now */
	bt_off();
}
int lui_busy_screen(void) { return s_screen != LUI_CLOSED && s_screen != LUI_HOME; }

void lui_open_home(const gba_rom_entry *roms, int nrom)
{
	s_roms = roms; s_nrom = nrom;
	s_screen = LUI_HOME;
	s_sel = 0;
	s_home_rom = -1;
}

void lui_open_home_game(const gba_rom_entry *roms, int nrom, int rom)
{
	lui_open_home(roms, nrom);
	s_home_rom = rom >= 0 && rom < nrom ? rom : -1;
}

/* Home's rows: Play together and Send (when it has a game), Join a game, Receive */
enum { HOME_PLAY, HOME_SEND, HOME_JOIN, HOME_RECEIVE };
static int home_rows(int *kind)
{
	int n = 0;
	if (s_home_rom >= 0) { kind[n++] = HOME_PLAY; kind[n++] = HOME_SEND; }
	kind[n++] = HOME_JOIN;
	kind[n++] = HOME_RECEIVE;
	return n;
}

/* ---- Send / Receive ---- */
#define SEND_FILES	4
static char s_files[SEND_FILES][XFER_PATH_MAX];
static int s_nfiles, s_file_i;
static int s_moved, s_kept;			/* files written on the other card, files it had already */
static unsigned int s_file_tick;		/* when the file going now started (its speed) */
static int s_relist;				/* received something: the card is listed again */
static int s_rcv_started;			/* a transfer was asked for on this link */

static void path_cat(char *d, unsigned int cap, const char *dir, const char *name)
{
	snprintf(d, cap, "%s/%s", dir, name);
}

#define GPT_MS(t)	((t) / 13000u)		/* 13 MHz ticks to milliseconds */

static void size_text(char *d, unsigned int cap, unsigned int b)
{
	if (b >= 1024u * 1024u) snprintf(d, cap, "%u.%u MB", b / (1024u * 1024u), (b % (1024u * 1024u)) * 10u / (1024u * 1024u));
	else snprintf(d, cap, "%u KB", (b + 1023u) / 1024u);
}

/* a transfer going on, as a view's status and bar: what, how far, how fast */
static void xfer_line(struct lui_view *v, const char *what)
{
	const struct pico_xfer_status *st = pico_xfer_status();
	unsigned int ms = GPT_MS(gpt4_get_current_tick() - s_file_tick);
	char a[16], b[16];
	size_text(a, sizeof a, st->done);
	size_text(b, sizeof b, st->size);
	if (st->size && ms > 300u && st->done)
		snprintf(s_line, sizeof s_line, "%s: %s of %s, %u KB/s", what, a, b,
			 (unsigned int)((unsigned long long)st->done / ms));
	else if (st->size)
		snprintf(s_line, sizeof s_line, "%s: %s of %s", what, a, b);
	else
		snprintf(s_line, sizeof s_line, "%s...", what);
	v->status = s_line;
	v->done = st->done;
	v->total = st->size;
}

/* what Send moves for a game: its file (found in its folder by where it starts on the card), its
 * battery save and a Game Boy cartridge's clock when there are any. How many. */
static int send_files(const gba_rom_entry *r)
{
	fat_vol v;
	fat_dir d;
	fat_dirent e;
	fat_file f;
	const char *dir = gba_sd_rom_dir(r);
	const char *sdir = r->type == GBA_CONSOLE_SNES ? "/saves/snes" : "/saves/gba";
	char nm[160];
	s_nfiles = 0;
	if (gba_sd_mount(&v) != 0)
		return 0;
	if (fat_opendir(&v, dir, &d) == 0)
		while (fat_readdir(&d, &e))
			if (!e.is_dir && e.first_clus == r->first_clus && e.size == r->size) {
				path_cat(s_files[s_nfiles++], XFER_PATH_MAX, dir, e.name);
				break;
			}
	if (!s_nfiles)
		return 0;
	gba_sd_file_name(r->name, r->type == GBA_CONSOLE_SNES ? "srm" : "sav", nm, sizeof nm, 0);
	path_cat(s_files[s_nfiles], XFER_PATH_MAX, sdir, nm);
	if (fat_open(&v, s_files[s_nfiles], &f) == 0)
		s_nfiles++;
	if (r->type == GBA_CONSOLE_GB || r->type == GBA_CONSOLE_GBC) {
		gba_sd_file_name(r->name, "rtc", nm, sizeof nm, 0);
		path_cat(s_files[s_nfiles], XFER_PATH_MAX, "/saves/gba", nm);
		if (fat_open(&v, s_files[s_nfiles], &f) == 0)
			s_nfiles++;
	}
	return s_nfiles;
}

void lui_open_send(const gba_rom_entry *roms, int nrom, int rom)
{
	s_roms = roms; s_nrom = nrom;
	s_rom = rom;
	if (rom < 0 || rom >= nrom || !send_files(&roms[rom])) {
		message(lt("Send"), lt("This game could not be read from the card."));
		return;
	}
	s_terms_send = 1;
	s_screen = LUI_TERMS;			/* the notice first; A goes on to the list */
}

void lui_open_receive(void)
{
	s_terms_send = 0;
	s_screen = LUI_TERMS;			/* the notice first; A starts waiting */
}
static void receive_start(void)
{
	s_screen = LUI_RECEIVE;
	s_moved = s_kept = 0;
	s_rcv_started = 0;
	s_file_tick = 0;
	bt_on();
	pico_link_offer(PICO_OFFER_RECEIVE, 0);
	pico_link_want(1);			/* wait for a link, advertising */
}

/* the end of a Send or a Receive: what happened, in words */
static void xfer_summary(const char *title, int sent)
{
	char why[96];
	if (!s_moved && s_kept)
		snprintf(why, sizeof why, "%s", sent ? lt("The other console has this game already.")
						      : lt("This console has these files already."));
	else if (s_kept)
		snprintf(why, sizeof why, lt("Copied: %d. Kept as they were: %d."), s_moved, s_kept);
	else
		snprintf(why, sizeof why, lt_n(s_moved, "%d file copied.", "%d files copied."), s_moved);
	message(title, why);
}

/* why a transfer failed, in words */
static const char *xfer_why(int err)
{
	switch (err) {
	case XFE_REFUSED: return lt("The other console has no room for it.");
	case XFE_CARD: return lt("The card could not take it.");
	case XFE_CHECKSUM: return lt("It arrived damaged and was not kept.");
	case XFE_CANCELLED: return lt("It was cancelled.");
	default: return lt("The link to the other console was lost.");
	}
}

static int send_step(unsigned int e)
{
	const struct pico_xfer_status *st = pico_xfer_status();
	if (e & LUI_B) {			/* stop: the other console is told, nothing half written */
		pico_xfer_cancel();
		bt_off();
		s_screen = LUI_CLOSED;
		return LUI_ACT_CLOSE;
	}
	if (s_file_i < 0) {			/* connecting */
		if (pico_link_state() == PICO_LINK_OPEN) {
			s_file_i = 0;
			s_file_tick = gpt4_get_current_tick();
			if (pico_xfer_send(s_files[0]) != 0)
				message(lt("Send"), xfer_why(XFE_LINK));
		} else if (pico_link_state() != PICO_LINK_OPENING)
			message(lt("Send"), lt("The other console could not be reached."));
		return LUI_ACT_NONE;
	}
	if (pico_xfer_busy())
		return LUI_ACT_NONE;
	if (st->state == XF_DONE) s_moved++;
	else if (st->state == XF_FAILED && st->error == XFE_THERE) s_kept++;
	else {
		message(lt("Send"), xfer_why(st->error));
		return LUI_ACT_NONE;
	}
	if (++s_file_i < s_nfiles) {
		s_file_tick = gpt4_get_current_tick();
		if (pico_xfer_send(s_files[s_file_i]) != 0)
			message(lt("Send"), xfer_why(XFE_LINK));
		return LUI_ACT_NONE;
	}
	pico_xfer_end();			/* that was all */
	xfer_summary(lt("Send"), 1);
	return LUI_ACT_NONE;
}

static int receive_step(unsigned int e)
{
	const struct pico_xfer_status *st = pico_xfer_status();
	if (e & LUI_B) {
		pico_xfer_cancel();
		bt_off();
		s_screen = LUI_CLOSED;
		return s_relist ? (s_relist = 0, LUI_ACT_RELIST) : LUI_ACT_CLOSE;
	}
	if (pico_xfer_busy())
		return LUI_ACT_NONE;
	if (s_rcv_started) {			/* the last one has ended */
		s_rcv_started = 0;
		if (st->state == XF_DONE) { s_moved++; s_relist = 1; }
		else if (st->state == XF_END) { xfer_summary(lt("Receive"), 0); return LUI_ACT_NONE; }
		else if (st->state == XF_FAILED && st->error == XFE_THERE) s_kept++;
		else if (st->state == XF_FAILED && (s_moved || s_kept) && pico_link_state() != PICO_LINK_OPEN) {
			xfer_summary(lt("Receive"), 0);	/* the sender left without its last word */
			return LUI_ACT_NONE;
		} else if (st->state == XF_FAILED) {
			message(lt("Receive"), xfer_why(st->error));
			return LUI_ACT_NONE;
		}
	}
	if (pico_link_state() == PICO_LINK_OPEN) {
		s_file_tick = gpt4_get_current_tick();
		if (pico_xfer_receive(1) == 0)
			s_rcv_started = 1;
	} else if (s_moved || s_kept) {
		/* files came and the sender has gone: its last word ('E') may still sit in the queue,
		 * unread, when it closes the link at once */
		xfer_summary(lt("Receive"), 0);
		return LUI_ACT_NONE;
	} else if (pico_link_state() == PICO_LINK_NONE) {
		static unsigned int asked;	/* a sender that left before sending: wait for the next
					 * (asked once a second: the service takes a moment to start it) */
		if (GPT_MS(gpt4_get_current_tick() - asked) > 1000u) {
			asked = gpt4_get_current_tick();
			pico_link_want(1);
		}
	}
	return LUI_ACT_NONE;
}

void lui_open_host(const gba_rom_entry *roms, int nrom, int rom)
{
	s_roms = roms; s_nrom = nrom;
	s_rom = rom;
	s_screen = LUI_HOST_WAIT;
	s_info_sent = 0;
	s_game_theirs[0] = 0;			/* the guest says its game and console in its 'J' */
	s_other_model = 0;
	s_my_id_ok = 0;
	if (!link_system(roms[rom].type)) {	/* both play this very game: say which, by its contents */
		fat_vol v;
		s_my_id_ok = gba_sd_mount(&v) == 0 && gba_rom_identity(&v, &roms[rom], &s_my_usize, &s_my_crc) == RA_OK;
	}
	bt_on();
	pico_link_offer(PICO_OFFER_HOST, roms[rom].type);
	pico_link_want(1);			/* wait for a link, advertising */
}

void lui_open_join(const gba_rom_entry *roms, int nrom)
{
	s_roms = roms; s_nrom = nrom;
	s_screen = LUI_JOIN_LIST;
	s_sel = 0;
	s_npeer = 0;
	s_host_system = -1;
	s_time_sent = 0;
	bt_on();
	pico_link_want(3);			/* list the consoles nearby */
}

/* this console's game of that system and name (the host's, for a lockstep session), or -1 */
static int rom_named(int type, const char *name)
{
	int i;
	for (i = 0; i < s_nrom; i++)
		if (s_roms[i].type == type && gba_sd_same_name(s_roms[i].name, name))
			return i;
	return -1;
}

/* this console's copy of the host's game: the one of that name when its identity agrees, else any
 * of that system with the same identity (zipped, plain, renamed); -2 a game of that name whose
 * contents differ (and none that agrees), -1 none. Without the host's identity, the name alone. */
static int rom_for_host(void)
{
	int named = rom_named(s_host_system, s_host_name), i;
	fat_vol v;
	uint32_t size, crc;
	if (!s_host_id_ok || gba_sd_mount(&v) != 0)
		return named;
	if (named >= 0 && gba_rom_identity(&v, &s_roms[named], &size, &crc) == RA_OK &&
	    size == s_host_usize && crc == s_host_crc)
		return named;
	for (i = 0; i < s_nrom; i++) {
		const gba_rom_entry *r = &s_roms[i];
		if (i == named || r->type != s_host_system || (!r->arc && r->size != s_host_usize))
			continue;
		if (gba_rom_identity(&v, r, &size, &crc) == RA_OK && size == s_host_usize && crc == s_host_crc)
			return i;
	}
	return named >= 0 ? -2 : -1;
}

void lui_set_painter(int (*paint)(const struct lui_progress *p)) { s_paint = paint; }
int lui_paint_progress(const struct lui_progress *p) { return s_paint && p ? s_paint(p) : 0; }

/* The time, both ways once a link opens: 'T', seconds since 1970 UTC (8 bytes, little endian),
 * then 1 when that clock holds a real time. A console whose clock was never set or lost its time
 * (pico_clock_is_set) takes the other's: the delay over the link is well under a second. */
static void time_send(void)
{
	unsigned char m[10];
	unsigned long long t = (unsigned long long)pico_clock_utc();
	unsigned int i;
	m[0] = 'T';
	for (i = 0; i < 8; i++) m[1 + i] = (unsigned char)(t >> (8 * i));
	m[9] = (unsigned char)pico_clock_is_set();
	if (pico_link_send(m, sizeof m) == 0)
		s_time_sent = 1;
}
static void time_take(const unsigned char *m, int n)
{
	unsigned long long t = 0;
	int i;
	if (n < 10 || !m[9] || pico_clock_is_set())
		return;
	for (i = 7; i >= 0; i--) t = t << 8 | m[1 + i];
	pico_clock_set_utc((long long)t);
}

/* a game's name as shown: the file name without its extension */
static void shown_name(char *d, unsigned int cap, const char *name)
{
	unsigned int n = 0, dot = 0;
	while (name[n] && n + 1 < cap) { d[n] = name[n]; if (name[n] == '.') dot = n; n++; }
	d[dot ? dot : n] = 0;
}

/* netplay's progress hook during a start: the step in words, then the theme draws */
static int start_progress(void)
{
	static const char *const steps[] = { "", "Waiting for the other console", "Sending your game",
					     "Receiving the other game", "Sending your save", "Receiving the other save",
					     "Starting" };
	struct lui_progress p;
	int phase;
	if (!s_paint)
		return 0;
	memset(&p, 0, sizeof p);
	pico_np_progress(&phase, &p.done, &p.total, &p.kbs);
	p.title = lt("Playing together");
	p.mine = s_game_mine;
	p.theirs = s_game_theirs[0] && strcmp(s_game_theirs, s_game_mine) ? s_game_theirs : NULL;
	p.other = lui_model_name(s_other_model);
	p.step_en = steps[phase >= 0 && phase <= PICO_NP_STARTING ? phase : 0];
	p.step = lt(p.step_en);
	p.title_en = "Playing together";
	if (p.kbs && p.total > p.done)
		p.secs_left = (p.total - p.done) / (p.kbs * 1024u) + 1u;
	return s_paint(&p);
}

static int launch(int rom, int host)
{
	if (rom >= 0)
		shown_name(s_game_mine, sizeof s_game_mine, s_roms[rom].uname);
	pico_np_set_progress(start_progress);	/* the start's screen, drawn by the theme */
	pico_np_arm(host, LUI_DELAY);		/* the launch starts a session (pico_netplay.c) */
	s_act_rom = rom;
	s_act_host = host;
	s_screen = LUI_CLOSED;			/* Bluetooth stays on: the session uses it */
	s_bt_on = 0;				/* it is the session's now (it turns it off at the end) */
	return LUI_ACT_LAUNCH;
}

/* the guest's answer to the host: 'J' JOIN_*, this console's model, its game's name as shown. One
 * that ends the link (any but JOIN_PLAYS: Bluetooth goes off next) is let out first. */
static void join_reply(int answer, int rom)
{
	unsigned char m[3 + 128];
	unsigned int n = 0;
	m[0] = 'J';
	m[1] = (unsigned char)answer;
	m[2] = (unsigned char)PICO_BT_MODEL_NO;
	if (answer == JOIN_PLAYS && rom >= 0) {
		while (s_roms[rom].uname[n] && n < 120u) n++;
		memcpy(m + 3, s_roms[rom].uname, n);
	}
	pico_link_send(m, 3 + n);
	if (answer != JOIN_PLAYS)
		pico_link_flush(300u);
}

/* the link's messages for these screens (a session's start takes the queue over later) */
static int host_step(void)
{
	unsigned char m[200];
	int n;
	if (s_info_sent && pico_link_state() != PICO_LINK_OPEN) {
		/* the friend left before playing (backed out, switched off): wait for the next one */
		s_info_sent = 0;
		s_game_theirs[0] = 0;
		s_other_model = 0;
		pico_link_want(1);
	}
	if (pico_link_state() == PICO_LINK_OPEN && !s_info_sent) {
		const gba_rom_entry *r = &s_roms[s_rom];
		time_send();	/* before the game: the guest moves on from its loop once it has that */
		unsigned int k = 0;
		m[0] = 'I';
		m[1] = r->type;
		put32(m + 2, r->size);
		while (r->name[k] && k < 120u) k++;
		memcpy(m + 6, r->name, k);
		if (s_my_id_ok) {	/* the name's end, then the identity */
			m[6 + k] = 0;
			put32(m + 7 + k, s_my_usize);
			put32(m + 11 + k, s_my_crc);
			k += 9;
		}
		if (pico_link_send(m, 6 + k) == 0)
			s_info_sent = 1;
	}
	while ((n = pico_link_recv(m, sizeof m)) > 0) {
		if (m[0] == 'T') { time_take(m, n); continue; }
		if (m[0] == 'J' && n >= 3) {
			unsigned int k = (unsigned)n - 3u < sizeof s_game_theirs - 1 ? (unsigned)n - 3u : sizeof s_game_theirs - 1;
			s_other_model = m[2];
			memcpy(s_game_theirs, m + 3, k);
			s_game_theirs[k] = 0;
			if (m[1] == JOIN_PLAYS)
				return launch(s_rom, 1);
			if (m[1] == JOIN_LEFT) {	/* still waiting: the next friend (the link
							 * closing brings the advertising back) */
				s_game_theirs[0] = 0;
				continue;
			}
			message(lt("Play together"), m[1] == JOIN_OTHER ? lt("The other console has another version of this game.") :
				lt("The other console does not have this game."));
			return LUI_ACT_NONE;
		}
	}
	return LUI_ACT_NONE;
}

static int guest_step(void)
{
	unsigned char m[200];
	int n;
	if (!s_time_sent && pico_link_state() == PICO_LINK_OPEN)
		time_send();
	while ((n = pico_link_recv(m, sizeof m)) > 0) {
		if (m[0] == 'T') { time_take(m, n); continue; }
		if (m[0] == 'I' && n >= 6 && s_host_system < 0) {
			unsigned int k = (unsigned)n - 6u < sizeof s_host_name - 1 ? (unsigned)n - 6u : sizeof s_host_name - 1;
			s_host_system = m[1];
			s_host_size = get32(m + 2);
			memcpy(s_host_name, m + 6, k);
			s_host_name[k] = 0;
			{	/* the identity after the name's end, when the host sent one */
				unsigned int nl = (unsigned int)strlen(s_host_name);
				s_host_id_ok = 6u + nl + 9u <= (unsigned int)n && m[6 + nl] == 0;
				if (s_host_id_ok) { s_host_usize = get32(m + 7 + nl); s_host_crc = get32(m + 11 + nl); }
			}
			shown_name(s_game_theirs, sizeof s_game_theirs, s_host_name);
			if (link_system(s_host_system)) {
				/* the host's game when this card has it (the same contents), else the
				 * guest's own game of that system from the library */
				int rom = rom_for_host();
				s_act_system = s_host_system;
				if (rom < 0) return LUI_ACT_PICK;
				s_same_rom = rom;
				s_sel = 0;
				s_screen = LUI_JOIN_CHOOSE;
				return LUI_ACT_NONE;
			} else {
				int rom = rom_for_host();
				if (rom == -2) {	/* the same name, other contents: playing would part */
					join_reply(JOIN_OTHER, -1);
					message(lt("Join a game"), lt("This console has another version of this game."));
					return LUI_ACT_NONE;
				}
				if (rom < 0) {		/* not on this card: copied by the player, if at all */
					join_reply(JOIN_CANNOT, -1);
					message(lt("Join a game"), lt("This game is not on this console. To play it together, copy it first: Link, Send on the other console and Link, Receive on this one."));
					return LUI_ACT_NONE;
				}
				join_reply(JOIN_PLAYS, rom);
				return launch(rom, 0);
			}
		}
	}
	return LUI_ACT_NONE;
}

void lui_picked(int rom)
{
	s_act_rom = -1;				/* launches only what this answer starts */
	if (s_screen != LUI_JOIN_WAIT)
		return;
	if (rom < 0) {				/* backed out: the host is told, nobody plays */
		join_reply(JOIN_LEFT, -1);
		bt_off();
		s_screen = LUI_CLOSED;
		return;
	}
	join_reply(JOIN_PLAYS, rom);
	launch(rom, 0);
}


int lui_input(unsigned int e)
{
	switch (s_screen) {
	case LUI_HOME: {
		int kind[4], n = home_rows(kind);
		if (e & LUI_UP) s_sel = s_sel > 0 ? s_sel - 1 : n - 1;
		if (e & LUI_DOWN) s_sel = s_sel + 1 < n ? s_sel + 1 : 0;
		if (e & LUI_B) { s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		if (e & LUI_A) {
			if (kind[s_sel] == HOME_PLAY) lui_open_host(s_roms, s_nrom, s_home_rom);
			else if (kind[s_sel] == HOME_SEND) lui_open_send(s_roms, s_nrom, s_home_rom);
			else if (kind[s_sel] == HOME_JOIN) lui_open_join(s_roms, s_nrom);
			else lui_open_receive();
		}
		return LUI_ACT_NONE;
	}
	case LUI_HOST_WAIT:
		if (e & LUI_B) { bt_off(); s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		return host_step();
	case LUI_JOIN_LIST: {
		struct pico_peer all[LUI_ROWS];
		int i, n = pico_link_peers(all, LUI_ROWS);
		s_npeer = 0;
		for (i = 0; i < n; i++)
			if (all[i].offer == PICO_OFFER_HOST)
				s_peer[s_npeer++] = all[i];
		if (s_sel >= s_npeer) s_sel = s_npeer ? s_npeer - 1 : 0;
		if (e & LUI_UP && s_npeer) s_sel = s_sel > 0 ? s_sel - 1 : s_npeer - 1;
		if (e & LUI_DOWN && s_npeer) s_sel = s_sel + 1 < s_npeer ? s_sel + 1 : 0;
		if (e & LUI_B) { bt_off(); s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		if (e & LUI_A && s_npeer) {
			pico_link_connect(s_peer[s_sel].addr);
			s_other_model = s_peer[s_sel].model;
			s_game_theirs[0] = 0;
			snprintf(s_sub, sizeof s_sub, "%s", lui_model_name(s_peer[s_sel].model));
			s_screen = LUI_JOIN_WAIT;
		}
		return LUI_ACT_NONE;
	}
	case LUI_JOIN_WAIT:
		if (e & LUI_B) { join_reply(JOIN_LEFT, -1); bt_off(); s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		if (s_host_system >= 0)
			return LUI_ACT_NONE;		/* picking (LUI_ACT_PICK sent): lui_picked answers */
		if (pico_link_state() != PICO_LINK_OPEN && pico_link_state() != PICO_LINK_OPENING) {
			message(lt("Join a game"), lt("The other console could not be reached."));
			return LUI_ACT_NONE;
		}
		return guest_step();
	case LUI_JOIN_CHOOSE:	/* the same game, or another of the system from the library */
		if (e & LUI_UP) s_sel = s_sel ? 0 : 1;
		if (e & LUI_DOWN) s_sel = s_sel ? 0 : 1;
		if (e & LUI_B) { join_reply(JOIN_LEFT, -1); bt_off(); s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		if (pico_link_state() != PICO_LINK_OPEN) {
			message(lt("Join a game"), lt("The other console could not be reached."));
			return LUI_ACT_NONE;
		}
		if (e & LUI_A) {
			if (s_sel == 0) {
				join_reply(JOIN_PLAYS, s_same_rom);
				return launch(s_same_rom, 0);
			}
			s_screen = LUI_JOIN_WAIT;	/* picking: lui_picked answers */
			return LUI_ACT_PICK;
		}
		return LUI_ACT_NONE;
	case LUI_SEND_LIST: {
		struct pico_peer all[LUI_ROWS];
		int i, n = pico_link_peers(all, LUI_ROWS);
		s_npeer = 0;
		for (i = 0; i < n; i++)
			if (all[i].offer == PICO_OFFER_RECEIVE)
				s_peer[s_npeer++] = all[i];
		if (s_sel >= s_npeer) s_sel = s_npeer ? s_npeer - 1 : 0;
		if (e & LUI_UP && s_npeer) s_sel = s_sel > 0 ? s_sel - 1 : s_npeer - 1;
		if (e & LUI_DOWN && s_npeer) s_sel = s_sel + 1 < s_npeer ? s_sel + 1 : 0;
		if (e & LUI_B) { bt_off(); s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		if (e & LUI_A && s_npeer) {
			pico_link_connect(s_peer[s_sel].addr);
			s_other_model = s_peer[s_sel].model;
			s_file_i = -1;
			s_moved = s_kept = 0;
			s_screen = LUI_SEND_RUN;
		}
		return LUI_ACT_NONE;
	}
	case LUI_SEND_RUN:
		return send_step(e);
	case LUI_TERMS:
		if (e & LUI_B) { s_screen = LUI_CLOSED; return LUI_ACT_CLOSE; }
		if (e & LUI_A) {	/* accepted: on to Send or Receive */
			if (s_terms_send) {
				s_screen = LUI_SEND_LIST;
				s_sel = 0;
				s_npeer = 0;
				bt_on();
				pico_link_want(3);	/* list the consoles nearby */
			} else
				receive_start();
		}
		return LUI_ACT_NONE;
	case LUI_RECEIVE:
		return receive_step(e);
	case LUI_MESSAGE:
		if (e & (LUI_A | LUI_B)) {
			s_screen = LUI_CLOSED;
			if (s_relist) { s_relist = 0; return LUI_ACT_RELIST; }
			return LUI_ACT_CLOSE;
		}
		return LUI_ACT_NONE;
	}
	return LUI_ACT_NONE;
}

void lui_view(struct lui_view *v)
{
	int i;
	memset(v, 0, sizeof *v);
	v->screen = s_screen;
	switch (s_screen) {
	case LUI_HOME: {
		int kind[4];
		v->title = lt("Link");
		v->nrows = home_rows(kind);
		for (i = 0; i < v->nrows; i++) {
			v->rows[i] = lt(kind[i] == HOME_PLAY ? "Play together" : kind[i] == HOME_SEND ? "Send" :
					kind[i] == HOME_JOIN ? "Join a game" : "Receive");
			if (kind[i] == HOME_PLAY) {
				shown_name(s_valbuf[i], sizeof s_valbuf[i], s_roms[s_home_rom].uname);
				v->values[i] = s_valbuf[i];
			}
		}
		v->sel = s_sel;
		if (s_home_rom < 0)
			v->status = lt("To play a game together or send it: X on it in the library.");
		v->nhints = 2;
		v->hint_code[0] = "A"; v->hint_label[0] = lt("Select");
		v->hint_code[1] = "B"; v->hint_label[1] = lt("Back");
		break;
	}
	case LUI_HOST_WAIT:
		v->title = lt("Play together");
		v->subtitle = s_rom >= 0 ? s_roms[s_rom].uname : NULL;
		v->status = pico_link_state() == PICO_LINK_OPEN ? lt("A friend joined. Starting...")
							       : lt("Waiting for a friend to join...");
		v->busy = 1;
		v->nhints = 1;
		v->hint_code[0] = "B"; v->hint_label[0] = lt("Cancel");
		break;
	case LUI_JOIN_LIST:
		v->title = lt("Join a game");
		for (i = 0; i < s_npeer; i++) {
			snprintf(s_rowbuf[i], sizeof s_rowbuf[i], "%s", lui_model_name(s_peer[i].model));
			snprintf(s_valbuf[i], sizeof s_valbuf[i], "%s", lui_system_name(s_peer[i].system));
			v->rows[i] = s_rowbuf[i];
			v->values[i] = s_valbuf[i];
		}
		v->nrows = s_npeer;
		v->sel = s_sel;
		v->status = s_npeer ? NULL : lt("Looking for consoles hosting a game...");
		v->busy = !s_npeer;
		v->nhints = s_npeer ? 2 : 1;
		if (s_npeer) { v->hint_code[0] = "A"; v->hint_label[0] = lt("Join"); }
		v->hint_code[v->nhints - 1] = "B"; v->hint_label[v->nhints - 1] = lt("Back");
		break;
	case LUI_JOIN_WAIT:
		v->title = lt("Join a game");
		v->subtitle = s_host_system >= 0 ? s_host_name : s_sub;
		v->status = s_host_system < 0 ? lt("Connecting...") : lt("Pick your game.");
		v->busy = 1;
		v->nhints = 1;
		v->hint_code[0] = "B"; v->hint_label[0] = lt("Cancel");
		break;
	case LUI_JOIN_CHOOSE:
		v->title = lt("Join a game");
		v->subtitle = s_game_theirs;
		v->rows[0] = s_roms[s_same_rom].uname[0] ? s_roms[s_same_rom].uname : s_roms[s_same_rom].name;
		v->values[0] = lt("Same game");
		v->rows[1] = lt("Choose another game");
		v->nrows = 2;
		v->sel = s_sel;
		v->status = lt("Play the game the other console plays, or another game to link with it.");
		v->nhints = 2;
		v->hint_code[0] = "A"; v->hint_label[0] = lt("OK");
		v->hint_code[1] = "B"; v->hint_label[1] = lt("Back");
		break;
	case LUI_SEND_LIST:
		v->title = lt("Send");
		v->subtitle = s_roms[s_rom].uname;
		for (i = 0; i < s_npeer; i++) {
			snprintf(s_rowbuf[i], sizeof s_rowbuf[i], "%s", lui_model_name(s_peer[i].model));
			v->rows[i] = s_rowbuf[i];
		}
		v->nrows = s_npeer;
		v->sel = s_sel;
		v->status = s_npeer ? NULL : lt("Looking for consoles ready to receive. On the other one: Link, Receive.");
		v->busy = !s_npeer;
		v->nhints = s_npeer ? 2 : 1;
		if (s_npeer) { v->hint_code[0] = "A"; v->hint_label[0] = lt("Send"); }
		v->hint_code[v->nhints - 1] = "B"; v->hint_label[v->nhints - 1] = lt("Back");
		break;
	case LUI_SEND_RUN:
	case LUI_RECEIVE: {
		const struct pico_xfer_status *st = pico_xfer_status();
		int moving = (s_screen == LUI_SEND_RUN ? s_file_i >= 0 : s_rcv_started) &&
			     (st->state == XF_SENDING || st->state == XF_RECEIVING || st->state == XF_WRITING);
		v->title = lt(s_screen == LUI_SEND_RUN ? "Send" : "Receive");
		v->subtitle = s_screen == LUI_SEND_RUN ? s_roms[s_rom].uname : NULL;
		v->busy = 1;
		if (moving) {
			const char *nm = st->path, *p;
			unsigned int ms = GPT_MS(gpt4_get_current_tick() - s_file_tick);
			char a[16], b[16];
			for (p = st->path; *p; p++) if (*p == '/') nm = p + 1;
			size_text(a, sizeof a, st->done);
			size_text(b, sizeof b, st->size);
			if (ms > 300u && st->done)
				snprintf(s_line, sizeof s_line, "%s: %s of %s, %u KB/s", nm, a, b,
					 (unsigned int)((unsigned long long)st->done / ms));
			else
				snprintf(s_line, sizeof s_line, "%s: %s of %s", nm, a, b);
			v->status = s_line;
			v->done = st->done;
			v->total = st->size;
		} else if (s_screen == LUI_SEND_RUN)
			v->status = lt(s_file_i < 0 ? "Connecting..." : "Waiting for the other console...");
		else
			v->status = lt(pico_link_state() == PICO_LINK_OPEN ? "A console is sending..."
									  : "Ready. On the other console: a game, Link, Send.");
		v->nhints = 1;
		v->hint_code[0] = "B"; v->hint_label[0] = lt(s_screen == LUI_SEND_RUN ? "Cancel" : "Close");
		break;
	}
	case LUI_TERMS:
		v->title = lt(s_terms_send ? "Send" : "Receive");
		v->status = lt("Only copy games you own. Copying a game you do not own may be against the law where you live. You copy at your own risk.");
		v->nhints = 2;
		v->hint_code[0] = "A"; v->hint_label[0] = lt("Accept");
		v->hint_code[1] = "B"; v->hint_label[1] = lt("Back");
		break;
	case LUI_MESSAGE:
		v->title = s_sub;
		v->status = s_line;
		v->nhints = 1;
		v->hint_code[0] = "A"; v->hint_label[0] = lt("OK");
		break;
	}
}

int lui_action_rom(void) { return s_act_rom; }
int lui_action_host(void) { return s_act_host; }
int lui_action_system(void) { return s_act_system; }

/* ---- in a game: the in-game menu's Netplay page ----
 * A link cable game (GB, GBC, GBA) can start a session where it stands, with no save going
 * anywhere: each console sends the other its machine as it is now (and its cartridge when the
 * other lacks it, pico_netplay.c), the two machines are wired together, and both play on. The
 * same messages as the Link screens ('T', 'I', 'J'), so a console in a game and one in the menu
 * find each other either way round: a host in a game takes a guest that joined from the Link
 * screen (which then launches its game for the session), and a guest in a game joins a host
 * waiting on the Link screen.
 * The page only asks: the runner starts the session at a point of its own between frames
 * (lui_game_start_wanted), ends it (lui_game_leave_wanted) and says how it went
 * (lui_game_session). Bluetooth is on only while hosting, finding, joining or playing. */
enum { GS_OFF, GS_HOSTING, GS_FINDING, GS_JOINING, GS_STARTING, GS_PLAYING };
static struct {
	int system;			/* the running game's, -1 no game (the page is not offered) */
	char name[128];			/* its file name on the card ('I') */
	char shown[128];		/* its name as shown ('J', the start's screen) */
	unsigned int size;		/* its file's size ('I') */
	int state;
	int start_req;			/* the runner is to start: 1 host, 0 guest, -1 nothing */
	int leave_req;
	int info_sent, time_sent;
	int sel;			/* GS_FINDING: the host picked among s_peer */
	char note[160];			/* the page's last word, in English (why a start failed, a session ended) */
	char value[96];
} G = { -1 };

/* the systems whose games link with the running one's (the two Game Boys share one core) */
static int gm_links_with(int system)
{
	if (G.system == 2) return system == 2;
	return system == 0 || system == 1;
}

static void gm_off(const char *note)
{
	if (G.state == GS_JOINING)
		{ unsigned char m[3] = { 'J', JOIN_LEFT, PICO_BT_MODEL_NO }; pico_link_send(m, 3); pico_link_flush(300u); }
	bt_off();
	G.state = GS_OFF;
	G.start_req = -1;
	if (note) snprintf(G.note, sizeof G.note, "%s", note);
}

void lui_game_begin(int system, const char *name, const char *shown, unsigned int size, int can)
{
	G.system = can && link_system(system) ? system : -1;
	G.size = size;
	snprintf(G.name, sizeof G.name, "%s", name ? name : "");
	snprintf(G.shown, sizeof G.shown, "%s", shown && shown[0] ? shown : G.name);
	G.state = GS_OFF;
	G.start_req = -1;
	G.leave_req = 0;
	G.note[0] = 0;
}

void lui_game_end(void)
{
	if (G.state != GS_PLAYING && G.state != GS_STARTING)
		gm_off(0);
	G.system = -1;
	G.state = GS_OFF;
}

void lui_game_session(int on, const char *why)
{
	if (on) {
		G.state = GS_PLAYING;
		G.note[0] = 0;
		s_bt_on = 0;			/* the session's now */
		return;
	}
	if (G.state == GS_PLAYING || G.state == GS_STARTING) {
		s_bt_on = 1;			/* whoever had it: off */
		bt_off();
	}
	G.state = GS_OFF;
	G.start_req = -1;
	G.leave_req = 0;
	if (why) snprintf(G.note, sizeof G.note, "%s", why);
}

int lui_game_start_wanted(void)
{
	int h = G.start_req;
	G.start_req = -1;
	return h;
}
int lui_game_leave_wanted(void)
{
	int l = G.leave_req;
	G.leave_req = 0;
	return l;
}
/* a session started from the Link screen (an armed launch) is the page's too: Leave ends it */
void lui_game_armed_session(void)
{
	if (G.system >= 0) {
		G.state = GS_PLAYING;
		G.note[0] = 0;
	}
}

/* the start's progress for the runner's own painter: the step in words, how far */
const char *lui_game_progress(void)
{
	static const char *const steps[] = { "", "Waiting for the other console", "Sending your game",
					     "Receiving the other game", "Sending your save", "Receiving the other save",
					     "Starting" };
	unsigned int done, total, kbs;
	int phase;
	pico_np_progress(&phase, &done, &total, &kbs);
	if (phase < 0 || phase > PICO_NP_STARTING) phase = 0;
	if (total && done <= total)
		snprintf(s_line, sizeof s_line, "%s  %u%%", lt(steps[phase]), (unsigned)((unsigned long long)done * 100u / total));
	else
		snprintf(s_line, sizeof s_line, "%s", lt(steps[phase]));
	return s_line;
}

static void gm_start(int host)
{
	pico_np_set_progress(0);		/* the runner paints its own start (lui_game_progress) */
	pico_np_arm(host, LUI_DELAY);
	G.start_req = host;
	G.state = GS_STARTING;
}

/* the host's side: a guest opened a link; our time and our game go, its answer comes */
static void gm_host_step(void)
{
	unsigned char m[200];
	int n;
	if (G.info_sent && pico_link_state() != PICO_LINK_OPEN) {	/* it left: wait for another */
		G.info_sent = 0;
		pico_link_want(1);
	}
	if (pico_link_state() == PICO_LINK_OPEN && !G.info_sent) {
		unsigned int k = 0;
		time_send();
		m[0] = 'I';
		m[1] = (unsigned char)G.system;
		put32(m + 2, G.size);
		while (G.name[k] && k < 120u) k++;
		memcpy(m + 6, G.name, k);
		if (pico_link_send(m, 6 + k) == 0)
			G.info_sent = 1;
	}
	while ((n = pico_link_recv(m, sizeof m)) > 0) {
		if (m[0] == 'T') { time_take(m, n); continue; }
		if (m[0] == 'J' && n >= 3) {
			s_other_model = m[2];
			if (m[1] == JOIN_PLAYS) { gm_start(1); return; }
			if (m[1] == JOIN_LEFT) continue;	/* still hosting: the next friend */
			snprintf(G.note, sizeof G.note, "%s", "The other console cannot play with this game.");
		}
	}
}

/* the guest's side: the link is open; the host's game says whether ours links with it */
static void gm_join_step(void)
{
	unsigned char m[200];
	int n;
	if (pico_link_state() != PICO_LINK_OPEN && pico_link_state() != PICO_LINK_OPENING) {
		gm_off("The other console could not be reached.");
		return;
	}
	if (!G.time_sent && pico_link_state() == PICO_LINK_OPEN) {
		time_send();
		G.time_sent = 1;
	}
	while ((n = pico_link_recv(m, sizeof m)) > 0) {
		if (m[0] == 'T') { time_take(m, n); continue; }
		if (m[0] == 'I' && n >= 6) {
			unsigned char r[3 + 128];
			unsigned int k = 0;
			r[0] = 'J';
			r[2] = (unsigned char)PICO_BT_MODEL_NO;
			if (!gm_links_with(m[1])) {
				r[1] = JOIN_CANNOT;
				pico_link_send(r, 3);
				pico_link_flush(300u);
				gm_off("The other console's game cannot link with this one.");
				return;
			}
			r[1] = JOIN_PLAYS;
			while (G.shown[k] && k < 120u) k++;
			memcpy(r + 3, G.shown, k);
			pico_link_send(r, 3 + k);
			gm_start(0);
			return;
		}
	}
}

/* each frame on the game's thread (menu_open: the in-game menu is up). Hosting, finding and
 * joining stop when the menu closes: a start never comes over a game being played. */
static void gm_debug_step(void);
void lui_game_tick(int menu_open)
{
	if (G.system < 0)
		return;
	if (menu_open) gm_debug_step();
	if (!menu_open && (G.state == GS_HOSTING || G.state == GS_FINDING || G.state == GS_JOINING)) {
		gm_off(0);
		return;
	}
	if (G.state == GS_HOSTING) gm_host_step();
	else if (G.state == GS_JOINING) gm_join_step();
}

/* the page's rows: Host, Join (the hosts nearby, Left / Right), or Leave in a session */
enum { GR_HOST, GR_JOIN, GR_LEAVE, GR_WITH, GR_NOTE };
static int gm_rows[4], gm_nrows;
static void gm_layout(void)
{
	gm_nrows = 0;
	if (G.state == GS_PLAYING || G.state == GS_STARTING) {
		gm_rows[gm_nrows++] = GR_LEAVE;
		gm_rows[gm_nrows++] = GR_WITH;
	} else {
		gm_rows[gm_nrows++] = GR_HOST;
		gm_rows[gm_nrows++] = GR_JOIN;
	}
	if (G.note[0] || G.state == GS_HOSTING || G.state == GS_JOINING)
		gm_rows[gm_nrows++] = GR_NOTE;
}
static void gm_peers(void)
{
	struct pico_peer all[LUI_ROWS];
	int i, n = pico_link_peers(all, LUI_ROWS);
	s_npeer = 0;
	for (i = 0; i < n; i++)
		if (all[i].offer == PICO_OFFER_HOST && gm_links_with(all[i].system))
			s_peer[s_npeer++] = all[i];
	if (G.sel >= s_npeer) G.sel = s_npeer ? s_npeer - 1 : 0;
}

int lui_game_page(void) { return G.system >= 0; }
void lui_game_page_open(void) { if (G.state != GS_FINDING) G.sel = 0; }
int lui_game_rows(void) { gm_layout(); return gm_nrows; }
const char *lui_game_row_title(int i)
{
	gm_layout();
	if (i < 0 || i >= gm_nrows) return "";
	switch (gm_rows[i]) {
	case GR_HOST:  return G.state == GS_HOSTING ? "Stop Hosting" : "Host a Session";
	case GR_JOIN:  return G.state == GS_FINDING ? "Join" : "Join a Session";
	case GR_LEAVE: return "Leave the Session";
	case GR_WITH:  return "Playing With";
	case GR_NOTE:  return G.state == GS_HOSTING ? "Waiting for a friend..." : G.state == GS_JOINING ? "Joining..." : G.note;
	default:       return "";
	}
}
/* (in the menus' language already) */
const char *lui_game_row_value(int i)
{
	gm_layout();
	if (i < 0 || i >= gm_nrows) return 0;
	switch (gm_rows[i]) {
	case GR_JOIN:
		if (G.state != GS_FINDING) return 0;
		gm_peers();
		if (!s_npeer) return lt("Looking...");
		snprintf(G.value, sizeof G.value, "%s", lui_model_name(s_peer[G.sel].model));
		return G.value;
	case GR_WITH:
		return lui_model_name(s_other_model);
	default:
		return 0;
	}
}
int lui_game_row_head(int i) { gm_layout(); return i >= 0 && i < gm_nrows && (gm_rows[i] == GR_WITH || gm_rows[i] == GR_NOTE); }
int lui_game_row_change(int i, int dir)
{
	gm_layout();
	if (i < 0 || i >= gm_nrows) return 0;
	switch (gm_rows[i]) {
	case GR_HOST:
		if (dir) return 0;
		if (G.state == GS_HOSTING) { gm_off(0); return 1; }
		{
			extern int pico_ra_hardcore(void) __attribute__((weak));
			if (pico_ra_hardcore && pico_ra_hardcore()) {
				snprintf(G.note, sizeof G.note, "%s", "Not in RetroAchievements hardcore");
				return 1;
			}
		}
		if (G.state != GS_OFF) gm_off(0);
		G.note[0] = 0;
		G.info_sent = 0;
		s_other_model = 0;
		bt_on();
		pico_link_offer(PICO_OFFER_HOST, G.system);
		pico_link_want(1);			/* wait for a link, advertising */
		G.state = GS_HOSTING;
		return 1;
	case GR_JOIN:
		if (G.state == GS_JOINING) return 0;
		if (G.state != GS_FINDING) {
			extern int pico_ra_hardcore(void) __attribute__((weak));
			if (dir) return 0;
			if (pico_ra_hardcore && pico_ra_hardcore()) {
				snprintf(G.note, sizeof G.note, "%s", "Not in RetroAchievements hardcore");
				return 1;
			}
			if (G.state != GS_OFF) gm_off(0);
			G.note[0] = 0;
			G.sel = 0;
			s_npeer = 0;
			bt_on();
			pico_link_want(3);		/* list the consoles nearby */
			G.state = GS_FINDING;
			return 1;
		}
		gm_peers();
		if (dir && s_npeer) { G.sel = (G.sel + (dir < 0 ? s_npeer - 1 : 1)) % s_npeer; return 1; }
		if (!dir && s_npeer) {
			pico_link_connect(s_peer[G.sel].addr);
			s_other_model = s_peer[G.sel].model;
			G.time_sent = 0;
			G.state = GS_JOINING;
			return 1;
		}
		return 0;
	case GR_LEAVE:
		if (dir || G.state != GS_PLAYING) return 0;
		G.leave_req = 1;
		return 1;
	default:
		return 0;
	}
}
/* what the page shows, as one number: it changes whenever the page should be drawn again */
unsigned int lui_game_sig(void)
{
	unsigned int h = 2166136261u, i;
	const char *p;
	if (G.state == GS_FINDING) gm_peers();
	h = (h ^ (unsigned)G.state) * 16777619u;
	h = (h ^ (unsigned)s_npeer) * 16777619u;
	h = (h ^ (unsigned)G.sel) * 16777619u;
	h = (h ^ (unsigned)s_other_model) * 16777619u;
	for (i = 0; i < (unsigned)s_npeer && G.state == GS_FINDING; i++)
		h = (h ^ (unsigned)s_peer[i].model ^ ((unsigned)s_peer[i].addr[0] << 8)) * 16777619u;
	for (p = G.note; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
	return h;
}

/* oem gnp[:h|j|l|n]: the Netplay page from USB, as its rows' A would (h Host or Stop, j Join: find,
 * then the host found first, l Leave), carried out by the next tick on the game's thread; and the
 * page as it stands (n: its note) */
static volatile char s_gm_dbg;
void lui_game_debug(const char *arg, char *out, int cap)
{
	static const char *const st[] = { "off", "hosting", "finding", "joining", "starting", "playing" };
	if (arg && (*arg == 'h' || *arg == 'j' || *arg == 'l')) s_gm_dbg = *arg;
	if (arg && *arg == 'n') {	/* the page's note, alone (a reply line holds 64) */
		snprintf(out, (unsigned)cap, "%.60s", G.note);
		return;
	}
	snprintf(out, (unsigned)cap, "gnp: sys %d %s peers %d sel %d model %d", G.system,
		 st[G.state >= 0 && G.state <= GS_PLAYING ? G.state : 0], s_npeer, G.sel, s_other_model);
}
static void gm_debug_step(void)
{
	char c = s_gm_dbg;
	int i;
	if (!c) return;
	s_gm_dbg = 0;
	gm_layout();
	for (i = 0; i < gm_nrows; i++) {
		int r = gm_rows[i];
		if ((c == 'h' && r == GR_HOST) || (c == 'j' && r == GR_JOIN) || (c == 'l' && r == GR_LEAVE)) {
			lui_game_row_change(i, 0);
			return;
		}
	}
}
