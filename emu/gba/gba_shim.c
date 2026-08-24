/*
 * Freestanding runtime shim for the gpSP GBA core running inside LK.
 *
 * LK already provides malloc/free/calloc, the mem and str core and printf, and
 * libgcc supplies the soft-float __aeabi_* helpers. This file fills the few
 * remaining gaps the core pulls in:
 *   - __clear_cache()  : the ARM dynarec flushes freshly written code here
 *   - a couple of libc helpers LK lacks (strtol/strcat/strrchr/realloc)
 *   - stdio/time stubs : the core still *links* load_gamepak()/load_bios()/
 *     load_backup() (file paths), but gba_wrap.c never calls them - it loads the
 *     ROM/BIOS/save straight from memory. The stubs just satisfy the linker.
 */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Cache sync for freshly-emitted dynarec code (self-modifying-code sequence).
 *
 * The dynarec calls this after writing a translated block, before executing it.
 * We must NOT use LK's arch_sync_cache_range here: it invalidates the WHOLE
 * I-cache (ICIALLU) on every block, which during gpSP's translate-heavy startup
 * runs the translator itself with a perpetually cold I-cache (looks like a hang),
 * and it also omits the trailing DSB/ISB/BPIALL that ARM requires before the new
 * code is fetched.
 *
 * Instead do the architecturally-correct, range-scoped maintenance over just the
 * emitted bytes: clean D-line to PoU, DSB, invalidate I-line to PoU, invalidate
 * branch predictor, DSB, ISB. LK runs single-core in PL1 so local (non-broadcast)
 * ops are sufficient. A 32-byte stride is <= the real line size, so at worst it
 * does redundant (still-correct) maintenance. */
#define GBA_CACHE_LINE 32u

void __clear_cache(void *start, void *end)
{
	unsigned long s = (unsigned long)start & ~(GBA_CACHE_LINE - 1);
	unsigned long e = (unsigned long)end;
	unsigned long a;

	if (e <= (unsigned long)start)
		return;

	for (a = s; a < e; a += GBA_CACHE_LINE)
		__asm__ __volatile__("mcr p15, 0, %0, c7, c11, 1" :: "r"(a) : "memory"); /* DCCMVAU: clean D to PoU */
	__asm__ __volatile__("dsb" ::: "memory");
	for (a = s; a < e; a += GBA_CACHE_LINE)
		__asm__ __volatile__("mcr p15, 0, %0, c7, c5, 1" :: "r"(a) : "memory");  /* ICIMVAU: invalidate I to PoU */
	__asm__ __volatile__("mcr p15, 0, %0, c7, c5, 6" :: "r"(0) : "memory");          /* BPIALL: invalidate branch predictor */
	__asm__ __volatile__("dsb\n\tisb" ::: "memory");
}

/* ---- small libc helpers LK does not export ---- */

long strtol(const char *s, char **endptr, int base)
{
	long v = 0;
	int neg = 0;
	if (!s) { if (endptr) *endptr = (char *)s; return 0; }
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2; base = 16;
	} else if (base == 0) {
		base = (s[0] == '0') ? 8 : 10;
	}
	for (;;) {
		int d;
		char c = *s;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
		else break;
		if (d >= base) break;
		v = v * base + d;
		s++;
	}
	if (endptr) *endptr = (char *)s;
	return neg ? -v : v;
}

/* Note: LK already provides strcat, strrchr and realloc - do not redefine them. */

/* ---- stdio / time stubs (linked, never called) ---- */

typedef struct __gba_FILE { int dummy; } FILE;

FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(FILE *f) { (void)f; return 0; }
size_t fread(void *p, size_t sz, size_t n, FILE *f) { (void)p; (void)sz; (void)n; (void)f; return 0; }
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f) { (void)p; (void)sz; (void)n; (void)f; return 0; }
int fseek(FILE *f, long off, int whence) { (void)f; (void)off; (void)whence; return -1; }
long ftell(FILE *f) { (void)f; return -1; }
int fgetc(FILE *f) { (void)f; return -1; }
char *fgets(char *s, int n, FILE *f) { (void)s; (void)n; (void)f; return NULL; }
int fputc(int c, FILE *f) { (void)c; (void)f; return -1; }
int fflush(FILE *f) { (void)f; return 0; }
void rewind(FILE *f) { (void)f; }
int feof(FILE *f) { (void)f; return 1; }

/* case-insensitive compare (cheat-file parsing; also handy generally) */
int strcasecmp(const char *a, const char *b)
{
	unsigned char ca, cb;
	do {
		ca = (unsigned char)*a++;
		cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
	} while (ca && ca == cb);
	return (int)ca - (int)cb;
}

/* The only sscanf use is inside the (never-loaded) cheat parser. A stub that
 * reports "nothing matched" is enough to link and is never reached at runtime. */
int sscanf(const char *str, const char *fmt, ...) { (void)str; (void)fmt; return 0; }

/* ---- real-time clock backing (Pokemon Emerald etc.) ----
 * time() returns seconds provided by the LK driver (wall clock since boot plus a
 * fixed base), and localtime() converts to a civil date/time (treated as UTC;
 * a handheld has no timezone). This is enough for the GBA RTC to advance. */
extern long gba_host_time(void);	/* gba_driver.c */

time_t time(time_t *t)
{
	time_t v = (time_t)gba_host_time();
	if (t) *t = v;
	return v;
}

struct tm *localtime(const time_t *tp)
{
	static struct tm tmv;
	long secs = tp ? (long)*tp : 0;
	long days = secs / 86400;
	long rem = secs % 86400;
	long z, era, doe, yoe, y, doy, mp, d, m;

	if (rem < 0) { rem += 86400; days--; }
	tmv.tm_hour = (int)(rem / 3600);
	tmv.tm_min  = (int)((rem % 3600) / 60);
	tmv.tm_sec  = (int)(rem % 60);
	/* day of week: 1970-01-01 was a Thursday (=4) */
	tmv.tm_wday = (int)(((days % 7) + 4 + 7 * 1000000L) % 7);

	/* civil date from days since epoch (Howard Hinnant's algorithm) */
	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;                         /* [0, 146096] */
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  /* [0, 365] */
	mp = (5 * doy + 2) / 153;                        /* [0, 11] */
	d = doy - (153 * mp + 2) / 5 + 1;                /* [1, 31] */
	m = mp + (mp < 10 ? 3 : -9);                      /* [1, 12] */
	y += (m <= 2);

	tmv.tm_mday = (int)d;
	tmv.tm_mon  = (int)m - 1;      /* [0,11] */
	tmv.tm_year = (int)(y - 1900);
	tmv.tm_yday = (int)doy;
	tmv.tm_isdst = 0;
	return &tmv;
}
