/*
 * pico_clock.c - the date and time (pico_clock.h).
 *
 * The MT6359's RTC keeps UTC (Android sets it from the network); a read is a handful of PMIC wrap
 * transfers, so the time is read once and then carried on from the LK's millisecond counter, and
 * read again every half minute. The time zone is a whole number of quarter hours, saved with the
 * settings; there is no daylight saving rule, it is set by hand like the time.
 */
#include <stdio.h>
#include <platform.h>              /* current_time: ms since the LK started */
#include <platform/mt_rtc.h>
#include <kernel/thread.h>
#include "pico_clock.h"
#include "pico_i18n.h"

extern int  ayaneo_get_tz_quarters(void);
extern void ayaneo_set_tz_quarters(int q);
extern int  ayaneo_get_clock12(void);
extern void ayaneo_set_clock12(int on);

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int pico_clock_days_in(int year, int mon)
{
	static const unsigned char k_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (mon < 1 || mon > 12) return 30;
	return k_days[mon - 1] + (mon == 2 && is_leap(year));
}

/* the dates the clock works with run 1970..2105: whole years and months are walked */
long long pico_clock_join(const pico_tm *tm)
{
	long long days = 0;
	int y, m;
	for (y = 1970; y < tm->year; y++) days += 365 + is_leap(y);
	for (m = 1; m < tm->mon; m++) days += pico_clock_days_in(tm->year, m);
	days += tm->mday - 1;
	return ((days * 24 + tm->hour) * 60 + tm->min) * 60 + tm->sec;
}
void pico_clock_split(long long t, pico_tm *tm)
{
	long long days;
	int secs;
	if (t < 0) t = 0;
	days = t / 86400; secs = (int)(t % 86400);
	tm->wday = (int)((days + 4) % 7);          /* 1970-01-01 was a Thursday */
	tm->hour = secs / 3600; tm->min = secs / 60 % 60; tm->sec = secs % 60;
	for (tm->year = 1970; days >= 365 + is_leap(tm->year); tm->year++) days -= 365 + is_leap(tm->year);
	for (tm->mon = 1; days >= pico_clock_days_in(tm->year, tm->mon); tm->mon++) days -= pico_clock_days_in(tm->year, tm->mon);
	tm->mday = (int)days + 1;
}

static long long s_base;                       /* UTC at s_base_ms */
static unsigned int s_base_ms;
static int s_have;

static void rtc_read_utc(void)
{
	struct rtc_time r;
	pico_tm tm;
	rtc_get_time(&r);
	tm.year = r.tm_year; tm.mon = r.tm_mon; tm.mday = r.tm_mday;
	tm.hour = r.tm_hour; tm.min = r.tm_min; tm.sec = r.tm_sec;
	if (tm.mon < 1 || tm.mon > 12 || tm.mday < 1 || tm.mday > 31 || tm.year < 1970) {   /* never set: 2026 */
		tm.year = 2026; tm.mon = 1; tm.mday = 1; tm.hour = tm.min = tm.sec = 0;
	}
	enter_critical_section();
	s_base = pico_clock_join(&tm);
	s_base_ms = (unsigned int)current_time();
	s_have = 1;
	exit_critical_section();
}

long long pico_clock_utc(void)
{
	unsigned int ms;
	long long t;
	if (!s_have || (unsigned int)current_time() - s_base_ms > 30000u) rtc_read_utc();
	enter_critical_section();
	ms = (unsigned int)current_time() - s_base_ms;
	t = s_base + ms / 1000u;
	exit_critical_section();
	return t;
}
long long pico_clock_local(void) { return pico_clock_utc() + (long long)pico_clock_zone() * 900; }
int  pico_clock_zone(void) { return ayaneo_get_tz_quarters(); }
void pico_clock_set_zone(int q) { ayaneo_set_tz_quarters(q); }
int  pico_clock_12h(void) { return ayaneo_get_clock12(); }
void pico_clock_set_12h(int on) { ayaneo_set_clock12(on); }

void pico_clock_set_local(const pico_tm *tm)
{
	struct rtc_time r;
	pico_tm u;
	pico_clock_split(pico_clock_join(tm) - (long long)pico_clock_zone() * 900, &u);
	r.tm_year = (unsigned short)u.year; r.tm_mon = (unsigned short)u.mon; r.tm_mday = (unsigned short)u.mday;
	r.tm_hour = (unsigned short)u.hour; r.tm_min = (unsigned short)u.min; r.tm_sec = (unsigned short)u.sec;
	rtc_set_time(&r);
	rtc_read_utc();
}

static const char *const k_month[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
					 "Oct", "Nov", "Dec" };
static const char *const k_wday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *pico_clock_month_name(int mon) { return tr(k_month[mon >= 1 && mon <= 12 ? mon - 1 : 0]); }

/* a language's form of a day or a time: %a the weekday, %d the day, %b the month's name, %m its
 * number, %Y the year; %H the hour (00..23), %h the hour on the 12 hour clock, %M the minutes, %p
 * AM or PM in the language */
static void clock_format(const pico_tm *tm, const char *f, char *buf, int cap)
{
	int n = 0;
	while (*f && n < cap - 1) {
		char part[32];
		const char *p = 0;
		if (f[0] != '%' || !f[1]) { buf[n++] = *f++; continue; }
		switch (f[1]) {
		case 'a': p = tr(k_wday[tm->wday]); break;
		case 'b': p = tr(k_month[tm->mon - 1]); break;
		case 'd': snprintf(part, sizeof part, "%d", tm->mday); p = part; break;
		case 'm': snprintf(part, sizeof part, "%d", tm->mon); p = part; break;
		case 'Y': snprintf(part, sizeof part, "%d", tm->year); p = part; break;
		case 'H': snprintf(part, sizeof part, "%02d", tm->hour); p = part; break;
		case 'h': snprintf(part, sizeof part, "%d", tm->hour % 12 ? tm->hour % 12 : 12); p = part; break;
		case 'M': snprintf(part, sizeof part, "%02d", tm->min); p = part; break;
		case 'p': p = tr(tm->hour < 12 ? "AM" : "PM"); break;
		default: part[0] = f[1]; part[1] = 0; p = part; break;
		}
		f += 2;
		while (*p && n < cap - 1) buf[n++] = *p++;
	}
	buf[n] = 0;
}
void pico_clock_time_text(long long local, char *buf, int cap)
{
	pico_tm tm;
	pico_clock_split(local, &tm);
	clock_format(&tm, pico_clock_12h() ? tr("%h:%M %p") : "%H:%M", buf, cap);
}
void pico_clock_date_text(long long local, char *buf, int cap)
{
	pico_tm tm;
	pico_clock_split(local, &tm);
	clock_format(&tm, tr("%a %d %b"), buf, cap);
}

void pico_clock_zone_text(int q, char *buf, int cap)
{
	int a = q < 0 ? -q : q;
	snprintf(buf, cap, "UTC%c%02d:%02d", q < 0 ? '-' : '+', a / 4, a % 4 * 15);
}
