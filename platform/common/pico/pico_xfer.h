/*
 * pico_xfer.h - files from one GammaOS Pico console's card to another's over the Bluetooth link
 * (pico_xfer.c). One file at a time, each on a worker thread; callers watch pico_xfer_status().
 */
#ifndef PICO_XFER_H
#define PICO_XFER_H

#define XFER_PATH_MAX		200

/* where a transfer is */
enum { XF_IDLE, XF_WAITING, XF_SENDING, XF_RECEIVING, XF_WRITING, XF_DONE, XF_FAILED,
       XF_END };			/* receiving: the sender said it has nothing more */

/* why one failed (pico_xfer_status()->error) */
enum {
	XFE_NONE,
	XFE_NO_FILE = 1,		/* sending: no such file on this card */
	XFE_READ = 2,			/* sending: the card could not be read */
	XFE_LINK = 3,			/* the link would not take a message */
	XFE_REFUSED = 4,		/* the other console turned it down (no room) */
	XFE_GONE = 5,			/* the other console never answered / the link closed */
	XFE_STALLED = 6,		/* nothing came for too long */
	XFE_CANCELLED = 7,		/* either side cancelled */
	XFE_ORDER = 8,			/* receiving: a piece out of order */
	XFE_THERE = 9,			/* the other console has a file of that name already: kept */
	XFE_CHECKSUM = 11,		/* the file arrived damaged: not written */
	XFE_CARD = 12,			/* the receiving card could not take it */
};

struct pico_xfer_status {
	int state;
	unsigned int size, done, ms;		/* bytes in all, bytes moved, time taken */
	int error;				/* XFE_* when it failed */
	char path[XFER_PATH_MAX];
};

const struct pico_xfer_status *pico_xfer_status(void);
int  pico_xfer_busy(void);			/* a transfer's thread is running */
/* send the card's file at path (it lands at the same path on the other card); 0 started */
int  pico_xfer_send(const char *path);
/* sending: tell the other console there is nothing more (it stops waiting); 0 sent */
int  pico_xfer_end(void);
/* wait for the other console's next file; accept 0 turns it down. A file the card has already is
 * turned down too (XFE_THERE on both sides): nothing is ever overwritten. 0 started */
int  pico_xfer_receive(int accept);
void pico_xfer_cancel(void);

#endif
