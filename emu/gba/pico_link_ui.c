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
static int s_home_rom = -1;		/* Home's Play together row: that game (-1: no such row) */
static int s_info_sent, s_bt_on;
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

/* the host's game crossing to a guest that lacks it (JOIN_NEED); on the guest, where it is:
 * 0 asking, 1 coming in, 2 written (the theme lists the card, then it joins) */
static int s_host_sending, s_copy;

/* the guest's answer to the host's game ('J'): it plays, it cannot (the game is not on its card),
 * or it left (backed out of picking, closed the screen): the host then waits for another */
enum { JOIN_PLAYS, JOIN_CANNOT, JOIN_LEFT,
       JOIN_NEED };			/* a lockstep game this card lacks: the host sends its file first */
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
	s_screen = LUI_SEND_LIST;
	s_sel = 0;
	s_npeer = 0;
	bt_on();
	pico_link_want(3);			/* list the consoles nearby */
}

void lui_open_receive(void)
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
	s_host_sending = 0;
	s_game_theirs[0] = 0;			/* the guest says its game and console in its 'J' */
	s_other_model = 0;
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
	bt_on();
	pico_link_want(3);			/* list the consoles nearby */
}

/* this console's game of that system and name (the host's, for a lockstep session), or -1 */
static int rom_named(int type, const char *name)
{
	int i;
	for (i = 0; i < s_nrom; i++)
		if (s_roms[i].type == type && !strcmp(s_roms[i].name, name))
			return i;
	return -1;
}

void lui_set_painter(int (*paint)(const struct lui_progress *p)) { s_paint = paint; }

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
	if (s_host_sending) {			/* the game going to the guest: the link is the transfer's */
		const struct pico_xfer_status *st = pico_xfer_status();
		if (pico_xfer_busy())
			return LUI_ACT_NONE;
		s_host_sending = 0;
		if (st->state != XF_DONE && !(st->state == XF_FAILED && st->error == XFE_THERE)) {
			message(lt("Play together"), xfer_why(st->error));
			return LUI_ACT_NONE;
		}				/* then the guest's 'J' as usual */
	}
	if (s_info_sent && pico_link_state() != PICO_LINK_OPEN) {
		/* the friend left before playing (backed out, switched off): wait for the next one */
		s_info_sent = 0;
		s_game_theirs[0] = 0;
		s_other_model = 0;
		pico_link_want(1);
	}
	if (pico_link_state() == PICO_LINK_OPEN && !s_info_sent) {
		const gba_rom_entry *r = &s_roms[s_rom];
		unsigned int k = 0;
		m[0] = 'I';
		m[1] = r->type;
		put32(m + 2, r->size);
		while (r->name[k] && k < 120u) k++;
		memcpy(m + 6, r->name, k);
		if (pico_link_send(m, 6 + k) == 0)
			s_info_sent = 1;
	}
	while ((n = pico_link_recv(m, sizeof m)) > 0) {
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
			if (m[1] == JOIN_NEED) {	/* the guest lacks the game: its file first */
				if (!send_files(&s_roms[s_rom]) || pico_xfer_send(s_files[0]) != 0) {
					message(lt("Play together"), lt("This game could not be read from the card."));
					return LUI_ACT_NONE;
				}
				s_file_tick = gpt4_get_current_tick();
				s_host_sending = 1;
				return LUI_ACT_NONE;
			}
			message(lt("Play together"), lt("The other console does not have this game."));
			return LUI_ACT_NONE;
		}
	}
	return LUI_ACT_NONE;
}

static int guest_step(void)
{
	unsigned char m[200];
	int n;
	while ((n = pico_link_recv(m, sizeof m)) > 0) {
		if (m[0] == 'I' && n >= 6 && s_host_system < 0) {
			unsigned int k = (unsigned)n - 6u < sizeof s_host_name - 1 ? (unsigned)n - 6u : sizeof s_host_name - 1;
			s_host_system = m[1];
			s_host_size = get32(m + 2);
			memcpy(s_host_name, m + 6, k);
			s_host_name[k] = 0;
			shown_name(s_game_theirs, sizeof s_game_theirs, s_host_name);
			if (link_system(s_host_system)) {
				s_act_system = s_host_system;
				return LUI_ACT_PICK;	/* the guest's own game of that system */
			} else {
				int rom = rom_named(s_host_system, s_host_name);
				if (rom < 0) {		/* not on this card: offer to copy it first */
					s_copy = 0;
					s_screen = LUI_JOIN_COPY;
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

/* the guest without the host's lockstep game: A copies it (it stays on this card, as Send would
 * leave it), the theme lists the card again, then it joins as usual */
static int copy_step(unsigned int e)
{
	const struct pico_xfer_status *st = pico_xfer_status();
	if (e & LUI_B) {			/* no: the host is told, nothing half written */
		if (pico_xfer_busy())
			pico_xfer_cancel();
		join_reply(JOIN_LEFT, -1);
		bt_off();
		s_screen = LUI_CLOSED;
		return LUI_ACT_CLOSE;
	}
	if (s_copy == 0) {
		if (pico_link_state() != PICO_LINK_OPEN) {
			message(lt("Join a game"), xfer_why(XFE_GONE));
			return LUI_ACT_NONE;
		}
		if (e & LUI_A) {
			s_file_tick = gpt4_get_current_tick();
			if (pico_xfer_receive(1) != 0) {
				message(lt("Join a game"), xfer_why(XFE_LINK));
				return LUI_ACT_NONE;
			}
			join_reply(JOIN_NEED, -1);
			s_copy = 1;
		}
		return LUI_ACT_NONE;
	}
	if (s_copy == 1) {
		if (pico_xfer_busy())
			return LUI_ACT_NONE;
		if (st->state != XF_DONE && !(st->state == XF_FAILED && st->error == XFE_THERE)) {
			join_reply(JOIN_LEFT, -1);
			message(lt("Join a game"), xfer_why(st->error));
			return LUI_ACT_NONE;
		}
		s_copy = 2;
		return LUI_ACT_RELIST;		/* the theme lists the card, then lui_set_roms */
	}
	{	/* listed again: the game is here now */
		int rom = rom_named(s_host_system, s_host_name);
		if (pico_link_state() != PICO_LINK_OPEN) {
			message(lt("Join a game"), xfer_why(XFE_GONE));
			return LUI_ACT_NONE;
		}
		if (rom < 0) {
			join_reply(JOIN_CANNOT, -1);
			message(lt("Join a game"), lt("The game was copied but is not in the library."));
			return LUI_ACT_NONE;
		}
		join_reply(JOIN_PLAYS, rom);
		return launch(rom, 0);
	}
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
	case LUI_JOIN_COPY:
		return copy_step(e);
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
		if (s_host_sending)
			xfer_line(v, lt("Copying the game to the other console"));
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
	case LUI_JOIN_COPY:
		v->title = lt("Join a game");
		v->subtitle = s_game_theirs;
		v->busy = s_copy != 0;
		if (s_copy == 0) {
			char a[16];
			size_text(a, sizeof a, s_host_size);
			snprintf(s_line, sizeof s_line, lt("This game is not on this console. Copy it from the other one (%s)? It stays on this card."), a);
			v->status = s_line;
			v->nhints = 2;
			v->hint_code[0] = "A"; v->hint_label[0] = lt("Copy");
			v->hint_code[1] = "B"; v->hint_label[1] = lt("Cancel");
			break;
		}
		if (s_copy == 1) xfer_line(v, lt("Copying the game"));
		else v->status = lt("Starting...");
		v->nhints = 1;
		v->hint_code[0] = "B"; v->hint_label[0] = lt("Cancel");
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
