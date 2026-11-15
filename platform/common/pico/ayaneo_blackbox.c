/*
 * Black box: a few event records kept on the SD card (/gammaos/blackbox.bin) so that an event
 * nobody watched - a fault in the night, the charging screen starting the system on its own - can
 * be read back after the next boot with `oem blackbox`. Four 512 B slots used as a ring by
 * sequence number. Nothing is written on the eMMC. A record made while no card is mounted (the
 * fault handler, the charging screen) waits in a watchdog proof DRAM page until a boot with the
 * card moves it (ayaneo_bb_boot_check).
 */
#include <string.h>
#include <printf.h>
#include <kernel/thread.h>
#include <arch/arm/mmu.h>
extern unsigned int gpt4_get_current_tick(void);

#define BB_DIR    "/gammaos"
#define BB_NAME   "blackbox.bin"       /* 4 x 512 B */
#define BB_SLOTS  4u
/* the card, through the SD driver (gba_driver.c); < 0 when no card is mounted. A build without
 * the SD driver has no card: these weak stubs keep the records in DRAM. */
int __attribute__((weak)) ayaneo_card_read(const char *dir, const char *name, void *buf, unsigned int len)
{ (void)dir; (void)name; (void)buf; (void)len; return -1; }
int __attribute__((weak)) ayaneo_card_write(const char *dir, const char *name, const void *buf, unsigned int len)
{ (void)dir; (void)name; (void)buf; (void)len; return -1; }
#define BB_MAGIC  0x58424241u          /* "ABBX" */

struct bb_rec {
	unsigned int magic, seq, type, up_ms, boot_reason;
	unsigned int pc, dfar, spsr, r[4], lr, sp;
	unsigned int stack[16];
	char thread[16], msg[48];
	unsigned int a[8];                 /* type specific values (see ayaneo_bb_note callers) */
};

static unsigned int s_bb_up_ms, s_bb_tick;
/* a running millisecond count that survives the 330 s wrap of the 13 MHz counter as long as it
 * is touched more than once per wrap (the LED thread and the charging screen call it) */
unsigned int ayaneo_bb_uptime_ms(void)
{
	unsigned int now = gpt4_get_current_tick();
	if (s_bb_tick) s_bb_up_ms += (now - s_bb_tick) / 13000u;
	s_bb_tick = now - ((now - s_bb_tick) % 13000u);
	return s_bb_up_ms;
}

/* into the card's ring; < 0 when there is no card to write */
static int bb_write(struct bb_rec *r)
{
	static unsigned char buf[512 * BB_SLOTS] __attribute__((aligned(64)));
	unsigned int i, max = 0, slot;
	memset(buf, 0, sizeof buf);
	if (ayaneo_card_read(BB_DIR, BB_NAME, buf, sizeof buf) >= 0)
		for (i = 0; i < BB_SLOTS; i++) {
			struct bb_rec *o = (struct bb_rec *)(buf + i * 512u);
			if (o->magic != BB_MAGIC) continue;
			if (o->seq > max) max = o->seq;
			/* the same event already in the ring (a DRAM record moved twice): keep one copy */
			if (o->type == r->type && o->up_ms == r->up_ms && o->pc == r->pc && o->dfar == r->dfar &&
			    !memcmp(o->msg, r->msg, sizeof o->msg))
				return 0;
		}
	r->magic = BB_MAGIC;
	r->seq = max + 1u;
	slot = r->seq % BB_SLOTS;
	memset(buf + slot * 512u, 0, 512);
	memcpy(buf + slot * 512u, r, sizeof *r);
	return ayaneo_card_write(BB_DIR, BB_NAME, buf, sizeof buf);
}

static int bb_boot_reason(void) { extern int ayaneo_boot_reason(void); return ayaneo_boot_reason(); }

/* A fault cannot write the eMMC: the storage driver needs interrupts and the scheduler, both
 * stopped in the abort handler (tried 2026-11-03: the write hung until the watchdog reset the
 * device). The handler writes the record to a DRAM slot instead, which keeps its content over
 * the watchdog reset, and the next boot moves it to the eMMC (ayaneo_bb_boot_check, LED thread).
 * The slot is the last 4 KB of the pstore carve-out (0x4D010000 + 0xE0000, reserved and mapped
 * by the LK; the RAM console itself is in SRAM at 0x0011D000 on this board). Measured on the
 * device: a marker there survives the watchdog reset, while the free DRAM below the rewind islands
 * (0x55FFF000, tried first) comes back as all ones. */
#define BB_RAM      ((volatile struct bb_rec *)0x4D0EF000u)
#define BB_RAM_NOTE ((volatile struct bb_rec *)0x4D0EF200u)   /* a note made with no card mounted */
#define BB_RAM_MAGIC 0x52424241u          /* "ABBR": a record waiting for the card */
extern void arch_clean_cache_range(addr_t start, size_t len);

/* The page is outside the LK's own RAM mapping (0x4C400000 + 9 MB): it was usable only when
 * the kernel dump code had mapped the pstore carve-out, which it does after a watchdog reset but
 * not on a cold power on (2026-11-03: the first boot after Android faulted on the boot loop
 * counter, then the fault handler faulted again writing its record). Map it before any use;
 * the fault handler only writes it once mapped. */
static volatile int s_bb_mapped;
static void bb_map(void)
{
	extern int arch_mmu_map(uint64_t paddr, vaddr_t vaddr, uint flags, uint count);
	if (s_bb_mapped) return;
	if (arch_mmu_map((uint64_t)0x4D0EF000u, (vaddr_t)0x4D0EF000u,
			 MMU_MEMORY_TYPE_NORMAL_WRITE_BACK | MMU_MEMORY_AP_P_RW_U_NA, 0x1000u) == 0)
		s_bb_mapped = 1;
}

static unsigned int bb_sum(const struct bb_rec *r)
{
	const unsigned int *w = (const unsigned int *)r;
	unsigned int i, s = 0x5A5A0000u;
	for (i = 2; i < sizeof *r / 4u; i++) s = (s << 5 | s >> 27) ^ w[i];
	return s;
}

/* fault handler: the registers it captured, the SVC stack words, the thread that faulted */
void ayaneo_bb_fault(const char *msg, unsigned pc, unsigned dfar, unsigned spsr,
		     const unsigned *r4, unsigned lr, unsigned sp)
{
	struct bb_rec *rp = (struct bb_rec *)BB_RAM;
	struct bb_rec r;
	unsigned int i;
	thread_t *t = current_thread;
	if (!s_bb_mapped) return;   /* the page is not mapped: writing it would fault again */
	memset(&r, 0, sizeof r);
	r.type = 1; r.up_ms = ayaneo_bb_uptime_ms(); r.boot_reason = (unsigned)bb_boot_reason();
	r.pc = pc; r.dfar = dfar; r.spsr = spsr; r.lr = lr; r.sp = sp;
	for (i = 0; i < 4; i++) r.r[i] = r4[i];
	if (sp >= 0x40000000u && sp < 0xFFFFFFC0u)
		for (i = 0; i < 16; i++) r.stack[i] = ((const unsigned *)sp)[i];
	if (t) strlcpy(r.thread, t->name, sizeof r.thread);
	strlcpy(r.msg, msg ? msg : "", sizeof r.msg);
	{ extern volatile int g_menu_live; extern int ayaneo_shot_source(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px);
	  unsigned pa, w, h, pt; r.a[0] = (unsigned)g_menu_live; r.a[1] = (unsigned)ayaneo_shot_source(&pa, &w, &h, &pt); }
	r.magic = BB_RAM_MAGIC;
	r.seq = bb_sum(&r);                 /* the seq field holds the checksum until the move */
	memcpy(rp, &r, sizeof r);
	arch_clean_cache_range((addr_t)rp, sizeof r);
}

/* a record waiting in DRAM goes to the card's ring; the DRAM copy is cleared only once it is there */
static void bb_move(volatile struct bb_rec *slot)
{
	struct bb_rec *rp = (struct bb_rec *)slot;
	struct bb_rec r;
	memcpy(&r, rp, sizeof r);
	if (r.magic != BB_RAM_MAGIC || r.seq != bb_sum(&r)) return;
	if (bb_write(&r) < 0) return;      /* no card: try again at the next boot */
	memset(rp, 0, sizeof r);
	arch_clean_cache_range((addr_t)rp, sizeof r);
	__asm__ volatile("dsb sy" : : : "memory");
}
/* every boot with the card: the records the last runs left in DRAM go to the card */
void ayaneo_bb_boot_check(void)
{
	bb_map();
	if (!s_bb_mapped) return;
	bb_move(BB_RAM);
	bb_move(BB_RAM_NOTE);
}

/* any other event: a short text and up to eight values */
void ayaneo_bb_note(unsigned type, const char *msg, const unsigned *a, unsigned n)
{
	static struct bb_rec r;
	unsigned int i;
	thread_t *t = current_thread;
	memset(&r, 0, sizeof r);
	r.type = type; r.up_ms = ayaneo_bb_uptime_ms(); r.boot_reason = (unsigned)bb_boot_reason();
	if (t) strlcpy(r.thread, t->name, sizeof r.thread);
	strlcpy(r.msg, msg ? msg : "", sizeof r.msg);
	for (i = 0; i < n && i < 8; i++) r.a[i] = a[i];
	if (bb_write(&r) >= 0) return;
	/* no card mounted (the charging screen): keep it in DRAM until a boot with the card */
	bb_map();
	if (!s_bb_mapped) return;
	r.magic = BB_RAM_MAGIC;
	r.seq = bb_sum(&r);
	memcpy((void *)BB_RAM_NOTE, &r, sizeof r);
	arch_clean_cache_range((addr_t)BB_RAM_NOTE, sizeof r);
}

/* oem blackbox: the records, oldest first, two or three lines each through emit() */
void ayaneo_bb_dump(void (*emit)(const char *), char *line, int n)
{
	static unsigned char buf[512 * BB_SLOTS] __attribute__((aligned(64)));
	unsigned int i, j, order[BB_SLOTS], cnt = 0;
	memset(buf, 0, sizeof buf);
	if (ayaneo_card_read(BB_DIR, BB_NAME, buf, sizeof buf) < 0) { emit("blackbox: no card, or no " BB_DIR "/" BB_NAME); return; }
	for (i = 0; i < BB_SLOTS; i++) if (((struct bb_rec *)(buf + i * 512u))->magic == BB_MAGIC) order[cnt++] = i;
	for (i = 1; i < cnt; i++) for (j = i; j > 0 && ((struct bb_rec *)(buf + order[j - 1] * 512u))->seq > ((struct bb_rec *)(buf + order[j] * 512u))->seq; j--) { unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
	if (!cnt) { emit("blackbox: empty"); return; }
	for (i = 0; i < cnt; i++) {
		struct bb_rec *r = (struct bb_rec *)(buf + order[i] * 512u);
		snprintf(line, n, "#%u type=%u up=%us boot=%u thr=%s %s", r->seq, r->type, r->up_ms / 1000u, r->boot_reason, r->thread, r->msg);
		emit(line);
		if (r->type == 1) {
			snprintf(line, n, " pc=%08x dfar=%08x spsr=%08x lr=%08x sp=%08x menu=%u game=%u", r->pc, r->dfar, r->spsr, r->lr, r->sp, r->a[0], r->a[1]);
			emit(line);
			snprintf(line, n, " r0-3 %08x %08x %08x %08x", r->r[0], r->r[1], r->r[2], r->r[3]);
			emit(line);
			for (j = 0; j < 16; j += 4) {
				snprintf(line, n, " sp+%02x %08x %08x %08x %08x", j * 4, r->stack[j], r->stack[j + 1], r->stack[j + 2], r->stack[j + 3]);
				emit(line);
			}
		} else {
			snprintf(line, n, " %u %u %u %u %u %u %u %u", r->a[0], r->a[1], r->a[2], r->a[3], r->a[4], r->a[5], r->a[6], r->a[7]);
			emit(line);
		}
	}
}

void ayaneo_bb_clear(void)
{
	static unsigned char z[512 * BB_SLOTS] __attribute__((aligned(64)));
	memset(z, 0, sizeof z);
	ayaneo_card_write(BB_DIR, BB_NAME, z, sizeof z);
}

/* Boot loop guard for the SD card emulator. The guard used to skip the card after ANY watchdog
 * reset; the RGU keeps that status over later reboots, so one crash left every boot after it on
 * a blank screen (2026-11-03). Now a counter in the same watchdog proof DRAM as the fault record
 * (the end of its 4 KB slot) counts SD boot attempts that did not reach a stable run: each start
 * of the SD path adds one, 30 s of running (LED thread) clears it. The card is skipped only when
 * the last reset was the watchdog AND two attempts in a row died before running stable. */
#define BB_GUARD       ((volatile unsigned int *)0x4D0EFF00u)
#define BB_GUARD_MAGIC 0x44524755u   /* "UGRD" */
static void bb_guard_put(unsigned int n)
{
	BB_GUARD[0] = BB_GUARD_MAGIC;
	BB_GUARD[1] = n;
	BB_GUARD[2] = ~n;
	arch_clean_cache_range((addr_t)0x4D0EFF00u, 64);
}
static unsigned int bb_guard_get(void)
{
	if (BB_GUARD[0] != BB_GUARD_MAGIC || BB_GUARD[2] != ~BB_GUARD[1]) return 0;   /* cold boot: random */
	return BB_GUARD[1];
}
volatile unsigned int g_dbg_sd_guard[2];   /* oem mode: attempts seen at this boot, watchdog status */
/* returns 1 when the SD path should be skipped this boot */
int ayaneo_sd_guard_enter(unsigned int wdt_status)
{
	unsigned int wdt = wdt_status & (0x80000000u | 0x40000000u | 0x20000000u);
	unsigned int n;
	bb_map();
	if (!s_bb_mapped) { g_dbg_sd_guard[0] = 0; g_dbg_sd_guard[1] = wdt_status; return 0; }   /* no counter: never skip */
	n = wdt ? bb_guard_get() : 0u;   /* a clean reset starts the count again */
	g_dbg_sd_guard[0] = n; g_dbg_sd_guard[1] = wdt_status;
	if (wdt && n >= 2u) return 1;
	bb_guard_put(n + 1u);
	return 0;
}
void ayaneo_sd_guard_stable(void) { if (s_bb_mapped) bb_guard_put(0u); }
