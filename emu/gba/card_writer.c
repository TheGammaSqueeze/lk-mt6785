/*
 * card_writer.c - whole files written to the card off the game's thread (card_writer.h).
 *
 * The files wait as copies in the writer's own memory (ayaneo_cardw_arena, 2 MB above the language
 * pack), laid one after another; it starts again from the bottom whenever the queue has emptied.
 * The thread runs at low priority, in the time the game leaves free each frame; the storage and
 * fat_wr locks keep it from meeting another card access halfway.
 */
#include <string.h>
#include <kernel/thread.h>
#include <kernel/event.h>
#include "card_writer.h"
#include "fat_wr.h"

extern void *ayaneo_cardw_arena(unsigned int *size);

#define CW_JOBS 8

struct cw_job {
	fat_vol *v;
	char dir[64], name[288];
	uint32_t off, len;
	char *status;
	int status_cap;
	const char *ok, *fail;
};

static struct cw_job s_job[CW_JOBS];
static volatile unsigned s_head, s_tail;      /* the next job to write, the next free place */
static volatile int s_busy;                   /* the thread is writing one now */
static uint32_t s_fill;                       /* the arena's first free byte (only the queuing side moves it) */
static uint8_t *s_arena;
static unsigned int s_arena_size;
static event_t s_ev;
static int s_thread_up;
volatile unsigned int g_dbg_cw_n, g_dbg_cw_ms, g_dbg_cw_fail;   /* oem persist: files written, the last's ms, failures */

static void copy_str(char *d, int cap, const char *s)
{
	int i;
	for (i = 0; i + 1 < cap && s && s[i]; i++) d[i] = s[i];
	d[i] = 0;
}

static int write_job(const struct cw_job *j)
{
	extern unsigned gpt4_get_current_tick(void);
	unsigned t0 = gpt4_get_current_tick();
	int rc;
	fat_wr_mkpath(j->v, j->dir);
	rc = fat_wr_put(j->v, j->dir, j->name, s_arena + j->off, j->len);
	g_dbg_cw_ms = (gpt4_get_current_tick() - t0) / 13000u;
	g_dbg_cw_n++;
	if (rc) g_dbg_cw_fail++;
	if (j->status) copy_str(j->status, j->status_cap, rc ? j->fail : j->ok);
	return rc;
}

static int writer_thread(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_ev);
		while (s_head != s_tail) {
			s_busy = 1;
			write_job(&s_job[s_head % CW_JOBS]);
			enter_critical_section();
			s_head++;
			exit_critical_section();
			s_busy = 0;
		}
	}
	return 0;
}

void card_writer_flush(void)
{
	int i;
	for (i = 0; i < 500 && (s_head != s_tail || s_busy); i++)
		thread_sleep(10);
}

int card_writer_put(fat_vol *v, const char *dir, const char *name, const void *buf, uint32_t len,
		    char *status, int status_cap, const char *ok, const char *fail)
{
	struct cw_job *j;
	if (!s_arena) s_arena = (uint8_t *)ayaneo_cardw_arena(&s_arena_size);
	if (!s_thread_up && s_arena) {
		thread_t *t;
		event_init(&s_ev, false, EVENT_FLAG_AUTOUNSIGNAL);
		t = thread_create("card_writer", &writer_thread, NULL, LOW_PRIORITY, 16384);
		if (t) { thread_resume(t); s_thread_up = 1; }
	}
	if (!s_thread_up || len > s_arena_size) {	/* no writer: on this thread, as before */
		int rc;
		fat_wr_mkpath(v, dir);
		rc = fat_wr_put(v, dir, name, buf, len);
		if (status) copy_str(status, status_cap, rc ? fail : ok);
		return rc ? -1 : 0;
	}
	/* room in the arena and a free place in the queue, else wait for the writer to catch up */
	if (s_fill + len > s_arena_size || s_tail - s_head >= CW_JOBS) card_writer_flush();
	enter_critical_section();
	if (s_head == s_tail && !s_busy) s_fill = 0;   /* all written: the arena is free again */
	exit_critical_section();
	if (s_fill + len > s_arena_size) {	/* the writer did not catch up in time: write it here */
		int rc;
		card_writer_flush();
		fat_wr_mkpath(v, dir);
		rc = fat_wr_put(v, dir, name, buf, len);
		if (status) copy_str(status, status_cap, rc ? fail : ok);
		return rc ? -1 : 0;
	}
	j = &s_job[s_tail % CW_JOBS];
	j->v = v;
	copy_str(j->dir, sizeof j->dir, dir);
	copy_str(j->name, sizeof j->name, name);
	j->off = s_fill;
	j->len = len;
	j->status = status; j->status_cap = status_cap; j->ok = ok; j->fail = fail;
	memcpy(s_arena + s_fill, buf, len);
	s_fill = (s_fill + len + 63u) & ~63u;
	enter_critical_section();
	s_tail++;
	exit_critical_section();
	event_signal(&s_ev, false);
	return 0;
}
