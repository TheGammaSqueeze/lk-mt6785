/*
 * pico_netplay.h - the start of a linked netplay session (pico_netplay.c), for the runners that
 * run both consoles' machines (the GB/GBC link cable).
 */
#ifndef PICO_NETPLAY_H
#define PICO_NETPLAY_H

/* what each console brings to a linked session and where the other's lands */
struct pico_np_pair {
	const void *mine;		/* this console's machine state */
	unsigned int msize;
	void *theirs;			/* the other's state lands here (cap bytes), *got its size */
	unsigned int cap;
	unsigned int *got;
	const void *rom;		/* this console's cartridge and how the core loads it */
	unsigned int rom_size, rom_flags;
	const char *rom_name;		/* its name on the card */
	/* the other console's game from this console's card, when it has it: name as the other's
	 * card has it, size and CRC-32 that must agree; into dst (cap bytes). 0 found and loaded.
	 * NULL: always over the air. */
	int (*find_rom)(const char *name, unsigned int size, unsigned int crc, void *dst, unsigned int cap);
	void *their_rom;		/* the other's cartridge lands here when the games differ */
	unsigned int rom_cap;
	unsigned int *their_rom_size;	/* 0: the same game, nothing was sent */
	unsigned int *their_rom_flags;
	/* memory free while the start runs, for packing and unpacking what crosses (zlib's tables
	 * and window, about 300 KB); NULL: everything crosses as it is */
	void *work;
	unsigned int work_size;
};

/* A reset of the game during a session: the host's menu asks (pico_np_ask_reset; only the host
 * may), the flag travels in the host's pad, and each runner, after pico_np_frame, takes it out of
 * player one's pad (pico_np_take_reset, 1 when set) and resets the game before that frame, on
 * both consoles at the same frame. */
#define PICO_NP_RESET 0x8000u
int  pico_np_is_host(void);
void pico_np_ask_reset(void);
int  pico_np_take_reset(unsigned int *p1);
/* the steps of a session's start, as its progress shows them (pico_np_progress) */
enum { PICO_NP_IDLE, PICO_NP_WAITING, PICO_NP_SEND_GAME, PICO_NP_RECV_GAME, PICO_NP_SEND_STATE,
       PICO_NP_RECV_STATE, PICO_NP_STARTING };
/* the function a theme's start screen is drawn by, at most every 100 ms during a start; it returns
 * 1 to cancel the start (B) */
void pico_np_set_progress(int (*fn)(void));
/* where the start is: its step, bytes done of total, KB/s */
void pico_np_progress(int *phase, unsigned int *done, unsigned int *total, unsigned int *kbs);

/* 0 the session started; both consoles call it, host 1 or 0 */
int pico_np_begin_pair(int host, unsigned int delay, const struct pico_np_pair *pp);
/* the runner's own part of a start in a game, for oem npstat: ms before pico_np_begin_pair (the
 * machine kept, the core made afresh) and after it (the two machines put together) */
void pico_np_note_prep(unsigned int before_ms, unsigned int after_ms);

#endif
