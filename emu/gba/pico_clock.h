/*
 * pico_clock.h - the date and time: the PMIC's real time clock (UTC, as Android keeps it), the
 * time zone set in Settings, local time for the cores' cartridge clocks and the menus.
 */
#ifndef PICO_CLOCK_H
#define PICO_CLOCK_H

typedef struct { int year, mon, mday, hour, min, sec, wday; } pico_tm;   /* mon 1..12, wday 0 Sunday */

long long pico_clock_utc(void);     /* seconds since 1970-01-01 UTC, from the RTC */
long long pico_clock_local(void);   /* the same in the time zone set */
int  pico_clock_zone(void);         /* the time zone, quarter hours east of UTC */
void pico_clock_set_zone(int quarters);
int  pico_clock_12h(void);
void pico_clock_set_12h(int on);
/* set the date and time of day in the time zone set: the RTC takes it (in UTC) */
void pico_clock_set_local(const pico_tm *tm);

void pico_clock_split(long long t, pico_tm *tm);
long long pico_clock_join(const pico_tm *tm);
int  pico_clock_days_in(int year, int mon);

/* "14:32" (or "2:32 PM" on the 12 hour clock), in the language in use */
void pico_clock_time_text(long long local, char *buf, int cap);
/* the day in the language's own form ("Thu 13 Nov") */
void pico_clock_date_text(long long local, char *buf, int cap);
/* "UTC+01:00" */
void pico_clock_zone_text(int quarters, char *buf, int cap);
/* a month's short name in the language in use (1..12) */
const char *pico_clock_month_name(int mon);

#endif
