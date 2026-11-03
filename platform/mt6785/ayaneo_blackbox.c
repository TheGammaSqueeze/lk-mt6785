/*
 * Black box: a few event records kept on the eMMC (boot_b, inside the partition) so that an
 * event nobody watched - a fault in the night, the charging screen starting the system on its
 * own - can be read back after the next boot with `oem blackbox`. Four 512 B slots used as a
 * ring by sequence number. Written from the fault handler (before the halt) and from the
 * offline charging screen when it hands over to the system. Read only on demand.
 */
#include <string.h>
#include <printf.h>
#include <kernel/thread.h>
#include <part_interface.h>
extern unsigned int gpt4_get_current_tick(void);

#define BB_PART   "boot_b"
#define BB_OFF    0x01FFC000u          /* 4 x 512 B, between the BIOS cache and the settings block */
#define BB_SLOTS  4u
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

static void bb_write(struct bb_rec *r)
{
	static unsigned char buf[512 * BB_SLOTS] __attribute__((aligned(64)));
	unsigned int i, max = 0, slot;
	if (partition_read(BB_PART, BB_OFF, buf, sizeof buf) == (ssize_t)sizeof buf)
		for (i = 0; i < BB_SLOTS; i++) {
			struct bb_rec *o = (struct bb_rec *)(buf + i * 512u);
			if (o->magic != BB_MAGIC) continue;
			if (o->seq > max) max = o->seq;
			/* the same event already in the ring (a DRAM record moved twice): keep one copy */
			if (o->type == r->type && o->up_ms == r->up_ms && o->pc == r->pc && o->dfar == r->dfar &&
			    !memcmp(o->msg, r->msg, sizeof o->msg))
				return;
		}
	r->magic = BB_MAGIC;
	r->seq = max + 1u;
	slot = r->seq % BB_SLOTS;
	memset(buf, 0, 512);
	memcpy(buf, r, sizeof *r);
	partition_write(BB_PART, BB_OFF + slot * 512u, buf, 512);
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
#define BB_RAM_MAGIC 0x52424241u          /* "ABBR": a fault record waiting for the eMMC */
extern void arch_clean_cache_range(addr_t start, size_t len);

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

/* every boot: a fault record left in DRAM by the last run goes to the eMMC ring */
void ayaneo_bb_boot_check(void)
{
	struct bb_rec *rp = (struct bb_rec *)BB_RAM;
	struct bb_rec r;
	memcpy(&r, rp, sizeof r);
	if (r.magic != BB_RAM_MAGIC || r.seq != bb_sum(&r)) return;
	memset(rp, 0, sizeof r);
	arch_clean_cache_range((addr_t)rp, sizeof r);
	__asm__ volatile("dsb sy" : : : "memory");
	bb_write(&r);
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
	bb_write(&r);
}

/* oem blackbox: the records, oldest first, two or three lines each through emit() */
void ayaneo_bb_dump(void (*emit)(const char *), char *line, int n)
{
	static unsigned char buf[512 * BB_SLOTS] __attribute__((aligned(64)));
	unsigned int i, j, order[BB_SLOTS], cnt = 0;
	if (partition_read(BB_PART, BB_OFF, buf, sizeof buf) != (ssize_t)sizeof buf) { emit("blackbox: read failed"); return; }
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
	partition_write(BB_PART, BB_OFF, z, sizeof z);
}
