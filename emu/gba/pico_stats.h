/*
 * pico_stats.h - how long each game has been played, and when it was last started.
 */
#ifndef PICO_STATS_H
#define PICO_STATS_H

/* a game starts (the menu picked it, or the console resumed it) */
void pico_stats_begin(int type, const char *name);
/* the game in play ends (back to the menu, or the power off): its time is added and saved */
void pico_stats_end(void);
/* the game picked did not start: its start is taken back (no play time, last played as before) */
void pico_stats_abort(void);
/* from a background thread now and then: a long session is saved every few minutes, so a battery
 * running flat loses little */
void pico_stats_tick(void);
/* a game's play time (seconds) and when it was last started (UTC seconds, 0 never); 0 if unknown */
int  pico_stats_get(int type, const char *name, unsigned int *secs, long long *last);

#endif
