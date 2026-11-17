/*
 * pico_link.h - what the Pico screens use of the Bluetooth link (pico_bt.inc): what this console
 * offers to the others, the consoles nearby, and opening a link to one of them.
 */
#ifndef PICO_LINK_H
#define PICO_LINK_H

/* what a console offers, in its advertising (pico_link_offer) */
enum { PICO_OFFER_NONE, PICO_OFFER_HOST, PICO_OFFER_RECEIVE };

/* the link's states (pico_link_state) */
enum { PICO_LINK_NONE, PICO_LINK_WAITING, PICO_LINK_FINDING, PICO_LINK_OPENING, PICO_LINK_OPEN,
       PICO_LINK_LISTING };

/* a console seen while listing */
struct pico_peer {
	unsigned char addr[6];
	unsigned char model;		/* 1 KPA, 2 Block */
	unsigned char offer;		/* PICO_OFFER_* */
	unsigned char system;		/* for a host: the game's system (GBA_CONSOLE_*) */
	signed char rssi;		/* dBm */
	unsigned int seen_tick;		/* when it was last heard (13 MHz counter) */
};

int  pico_bt_service_start(void);	/* Bluetooth on in the background (0 started) */
void pico_bt_service_stop(void);
int  pico_link_state(void);
/* 0 nothing / close, 1 wait for a link (connectable advertising), 2 find a Pico and open a link,
 * 3 list the Picos nearby (no link) */
void pico_link_want(int what);
/* what this console offers while it waits (advertised; system for a host) */
void pico_link_offer(int offer, int system);
/* the consoles heard while listing, the freshest first; how many (at most max) */
int  pico_link_peers(struct pico_peer *out, int max);
/* open a link to that console (from the list) */
void pico_link_connect(const unsigned char addr[6]);
/* wait up to ms for everything queued to have left (0 all out): a last word before closing */
int  pico_link_flush(unsigned int ms);

#endif
