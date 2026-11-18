/*
 * pico_netsvc.h - the network service of the consoles with Wi-Fi (the Konkr Pocket Block and
 * Advance): one low-priority thread owns the radio and does the slow work (starting Wi-Fi,
 * scanning, joining, DHCP, HTTP and HTTPS requests); the menu and the RetroAchievements code ask
 * it through these calls, which never block, and read its state.
 *
 * The card is never touched by the service: the menu thread loads and stores the saved networks
 * (netsvc_saved_load / netsvc_saved_store) when it wants.
 */
#ifndef PICO_NETSVC_H
#define PICO_NETSVC_H

#include <stdint.h>

enum {
	NETSVC_OFF,		/* the radio is off */
	NETSVC_STARTING,	/* the radio starting (its firmware loading) */
	NETSVC_READY,		/* on, not joined */
	NETSVC_SCANNING,
	NETSVC_JOINING,		/* joining, then asking for an address */
	NETSVC_ONLINE,		/* joined, with an address */
	NETSVC_FAILED		/* the radio did not start: why says why */
};

/* a network heard in the last scan */
struct netsvc_ap {
	char ssid[33];
	signed char dbm;
	unsigned char channel;
	unsigned char secured;		/* needs a password */
	unsigned char usable;		/* 0: a kind of security we cannot join yet (WPA3-only, enterprise, WEP) */
	unsigned char saved;		/* in the saved networks */
};

struct netsvc_status {
	int state;			/* NETSVC_* */
	const char *why;		/* the last failure (a join, the radio), "" none */
	char ssid[33];			/* joined / joining */
	int dbm;
	uint32_t ip, gateway, dns;	/* when online */
	unsigned int scan_gen;		/* moves on with every finished scan */
	unsigned int saved_gen;		/* moves on when the saved networks change (store them) */
};

/* the radio on (a saved network in range is joined on its own) or off */
void netsvc_wifi(int on);
/* a fresh scan (when on and not joined) */
void netsvc_scan(void);
/* the last scan's networks, strongest first: how many */
int netsvc_aps(struct netsvc_ap *out, int max);
/* join ssid: with pass (8..63 characters; "" for an open network), or pass 0 for a saved one */
void netsvc_join(const char *ssid, const char *pass);
/* leave the network (the radio stays on) */
void netsvc_leave(void);
void netsvc_get(struct netsvc_status *st);

/* the saved networks: names only for the menu; forget one */
int netsvc_saved_names(char names[][33], int max);
void netsvc_forget(const char *ssid);
/* the saved networks from / to the card (the menu thread only; a fat_vol *) */
void netsvc_saved_load(void *vol);
void netsvc_saved_store(void *vol);

/* HTTP(S), one request at a time. begin: 0 queued, -1 busy or not online. poll: 0 still
 * running, 1 done (*status, the body at *body, *len), -1 failed (*why). The body stays valid
 * until release, which frees the service for the next request. */
int  netsvc_http_begin(const char *host, const char *path, int secure, const void *post, unsigned int post_len);
int  netsvc_http_poll(unsigned int *status, const unsigned char **body, unsigned int *len, const char **why);
void netsvc_http_release(void);
/* the menu thread sets the clock from the network's time when a join brought one and the RTC
 * is unset or more than 2 s off: 1 when it did (the RTC is only touched from one thread) */
int  netsvc_clock_apply(void);
/* the menu thread gives the service the time now and then (the RTC is read on that thread only;
 * the service adds the time since): before any HTTPS, and every few seconds */
void netsvc_time_hint(void);

/* the user agent for HTTP requests (RetroAchievements wants its own) */
void netsvc_user_agent(const char *ua);

/* 1 while a game is played: the service sleeps and costs the game nothing; 0 the menu again */
void netsvc_quiet(int on);

/* secrets kept on the card, masked for this console (label: what they are); again to unmask */
void netsvc_mask(void *data, unsigned int len, const char *label);

#endif
