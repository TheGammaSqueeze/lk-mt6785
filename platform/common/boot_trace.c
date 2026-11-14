/*
 * boot_trace.c - bring-up trace for a board with no serial console (BOOT_TRACE builds).
 *
 * Every character the LK logs is mirrored into the last 64 KB of the pstore carve-out, mapped
 * uncached so it is in DRAM the moment it is written. That RAM keeps its content over a watchdog
 * reset, so the next boot finds the whole log of the boot before.
 *
 * A trace build starts in fastboot. `fastboot oem tryboot` asks for one normal boot: the board
 * resets, the next LK boots the system with the watchdog armed in reset mode, and wherever it
 * stops, the watchdog brings the board back to fastboot, where `fastboot oem trace` prints the
 * log of that attempt and `fastboot oem lastkmsg` the kernel's console (ramoops) if the kernel
 * was reached.
 *
 * When the boot before stopped before fastboot came up at all, the next boot brings fastboot up
 * straight after the storage (platform_init, boot_trace_rescue_fastboot in mt_boot.c) and waits
 * there, so even a hang early in the platform set up can be read back.
 */
#include <string.h>
#include <printf.h>
#include <debug.h>
#include <arch/arm/mmu.h>
#include <arch/ops.h>
#include <kernel/timer.h>
#include <part_interface.h>
#include <platform/boot_mode.h>

#define TRACE_BASE      0x4D0E0000u   /* pstore 0x4D010000 + 0xE0000: its last 64 KB (pmsg) */
#define TRACE_SIZE      0x10000u
#define KMSG_BASE       0x4D0A0000u   /* ramoops console zone (0x40000) */
#define KMSG_SIZE       0x40000u
#define TRACE_MAGIC     0x42525447u   /* "GTRB" */
#define RAMOOPS_SIG     0x43474244u   /* "DBGC" */

#define TF_TRY_PENDING  0x1u          /* the next boot is a normal boot attempt */
#define TF_TRY          0x2u          /* this boot is that attempt */
#define TF_FASTBOOT     0x4u          /* reached fastboot */
#define TF_KERNEL       0x8u          /* jumped to the kernel */
#define TF_POWEROFF     0x10u         /* the LK powered the board off (no charger, low battery) */

struct trace_hdr {
	unsigned int magic, seq, flags, head, wrapped, stage, stage_ms, reserved[9];
	char extra_args[192];     /* kernel arguments added to the next boot attempt (oem tryboot:...) */
};
#define RING_SIZE (TRACE_SIZE - sizeof(struct trace_hdr))

static volatile struct trace_hdr *s_hdr;
static volatile char *s_ring;
static int s_ready;

static struct trace_hdr s_last_hdr;
static char s_last[RING_SIZE];
static int s_have_last;
static unsigned int s_prev_flags;

extern int arch_mmu_map(uint64_t paddr, vaddr_t vaddr, uint flags, uint count);

void boot_trace_init(void)
{
	unsigned int prev_flags = 0, prev_seq = 0;

	if (arch_mmu_map((uint64_t)TRACE_BASE, (vaddr_t)TRACE_BASE,
			 MMU_MEMORY_TYPE_NORMAL | MMU_MEMORY_AP_P_RW_U_NA, TRACE_SIZE) != 0)
		return;
	s_hdr = (volatile struct trace_hdr *)TRACE_BASE;
	s_ring = (volatile char *)(TRACE_BASE + sizeof(struct trace_hdr));

	if (s_hdr->magic == TRACE_MAGIC && s_hdr->head < RING_SIZE) {
		memcpy(&s_last_hdr, (const void *)s_hdr, sizeof(s_last_hdr));
		memcpy(s_last, (const void *)s_ring, RING_SIZE);
		s_have_last = 1;
		prev_flags = s_hdr->flags;
		prev_seq = s_hdr->seq;
	}
	s_hdr->magic = TRACE_MAGIC;
	s_hdr->seq = prev_seq + 1;
	s_hdr->flags = (prev_flags & TF_TRY_PENDING) ? TF_TRY : 0;
	if (!(prev_flags & TF_TRY_PENDING))   /* extra arguments last for the one attempt */
		s_hdr->extra_args[0] = '\0';
	s_hdr->extra_args[sizeof(s_hdr->extra_args) - 1] = '\0';
	s_hdr->head = 0;
	s_hdr->wrapped = 0;
	s_hdr->stage = 0;
	s_hdr->stage_ms = 0;
	s_hdr->reserved[0] = *(volatile unsigned int *)0x1000700Cu;   /* TOPRGU status: why the last reset */
	arch_clean_cache_range((addr_t)s_hdr, sizeof(struct trace_hdr));
	s_ready = 1;
	{	/* the MediaTek pal_log_info lines too, not only the warnings */
		extern void set_log_level(uint32_t log_level);
		set_log_level(3);
	}
	s_prev_flags = prev_flags;
}

/* What the trace knows about this boot, into the log. Not from boot_trace_init: that runs
 * before the UART is set up, and a log line then waits on it forever. */
void boot_trace_report(void)
{
	unsigned int vbar;

	if (!s_ready)
		return;
	__asm__ volatile("mrc p15, 0, %0, c12, c0, 0" : "=r"(vbar));
	dprintf(CRITICAL, "[TRACE] boot %u, reset status 0x%x, previous boot flags 0x%x, VBAR 0x%x\n",
		s_hdr->seq, s_hdr->reserved[0], s_prev_flags, vbar);
}

/* ---- the boot state on the eMMC ----
 * What decides how a boot goes (a boot attempt asked for, and how far the boot before got) is
 * kept in 512 bytes of misc at +0x3F000, not in the DRAM header: across a software reset the
 * header did not hold what the boot before wrote into it. The record is read once the storage is
 * up (boot_trace_storage_up, early in platform_init) and written at each change. */
#define STATE_PART     "misc"
#define STATE_OFFSET   0x3F000u
#define STATE_MAGIC    0x54534754u   /* "GTST" */

enum { PROGRESS_NONE, PROGRESS_STARTED, PROGRESS_FASTBOOT, PROGRESS_KERNEL, PROGRESS_POWEROFF };

struct trace_state {
	unsigned int magic, try_pending, progress, seq;
	char extra_args[192];
	unsigned char pad[512 - 16 - 192];
};

static struct trace_state s_state __attribute__((aligned(64)));
static int s_state_ok;              /* the record was read (or created) */
static unsigned int s_prev_progress = PROGRESS_NONE;
static int s_is_try;                /* this boot is the boot attempt the record asked for */
static char s_try_args[192];        /* and its extra kernel arguments */

static void state_write(void)
{
	if (!s_state_ok)
		return;
	if (partition_write(STATE_PART, STATE_OFFSET, (u8 *)&s_state, sizeof(s_state)) != (ssize_t)sizeof(s_state))
		dprintf(CRITICAL, "[TRACE] writing the boot state failed\n");
}

static void state_progress(unsigned int progress)
{
	if (s_state_ok && s_state.progress != progress) {
		s_state.progress = progress;
		state_write();
	}
}

void boot_trace_storage_up(void)
{
	if (!s_ready)
		return;
	if (partition_read(STATE_PART, STATE_OFFSET, (u8 *)&s_state, sizeof(s_state)) != (ssize_t)sizeof(s_state)) {
		dprintf(CRITICAL, "[TRACE] reading the boot state failed\n");
		return;
	}
	s_state_ok = 1;
	if (s_state.magic != STATE_MAGIC) {
		memset(&s_state, 0, sizeof(s_state));
		s_state.magic = STATE_MAGIC;
	}
	s_prev_progress = s_state.progress;
	s_state.seq++;
	s_hdr->seq = s_state.seq;
	if (s_state.try_pending) {
		s_is_try = 1;
		memcpy(s_try_args, s_state.extra_args, sizeof(s_try_args));
		s_try_args[sizeof(s_try_args) - 1] = '\0';
		s_hdr->flags |= TF_TRY;
		s_state.try_pending = 0;     /* the attempt is made once */
	} else {
		s_is_try = 0;
		s_try_args[0] = '\0';
		s_hdr->flags &= ~TF_TRY;
	}
	s_state.progress = PROGRESS_STARTED;
	state_write();
	dprintf(CRITICAL, "[TRACE] boot %u: the boot before got to %s%s\n", s_state.seq,
		s_prev_progress == PROGRESS_KERNEL ? "the kernel" :
		s_prev_progress == PROGRESS_POWEROFF ? "a power off by the LK" :
		s_prev_progress == PROGRESS_FASTBOOT ? "fastboot" :
		s_prev_progress == PROGRESS_STARTED ? "neither fastboot nor the kernel" : "(no record)",
		s_is_try ? "; this boot is a boot attempt" : "");
}

/* 1 when the boot before this one stopped short of both fastboot and the kernel, a hang the
 * watchdog had to end: this boot then offers fastboot early (boot_trace_rescue_fastboot) so the
 * log of that boot can be read. */
int boot_trace_prev_failed(void)
{
	/* The boot state on the eMMC says how far the boot before got. Without it: a watchdog
	 * timeout in the TOPRGU status (the preloader usually clears it), or the RAM log. */
	unsigned int rgu = s_ready ? s_hdr->reserved[0] : 0;

	if (s_state_ok)	/* the boot state on the eMMC: a boot that reached the kernel is not a failure */
		return s_prev_progress == PROGRESS_STARTED;
	if (rgu & (0x80000000u | 0x20000000u))
		return 1;
	return s_have_last && !(s_last_hdr.flags & (TF_FASTBOOT | TF_KERNEL | TF_POWEROFF));
}

void boot_trace_putc(char c)
{
	unsigned int h;

	if (!s_ready)
		return;
	h = s_hdr->head;
	s_ring[h] = c;
	if (++h >= RING_SIZE) {
		h = 0;
		s_hdr->wrapped = 1;
	}
	s_hdr->head = h;
}

void boot_trace_stage(unsigned int stage, unsigned int ms)
{
	if (!s_ready)
		return;
	s_hdr->stage = stage;
	s_hdr->stage_ms = ms;
}

/* 1 when this boot is the one normal boot attempt asked for by oem tryboot */
int boot_trace_is_try(void)
{
	if (s_state_ok)
		return s_is_try;
	return s_ready && (s_hdr->flags & TF_TRY);
}

/* Push the trace window out of the data cache. The window is mapped non-cacheable, but it sits
 * in DRAM the LK has already mapped cacheable as a larger block, and a header flag written
 * through that mapping stayed in the cache over a software reset: the next boot read the flags
 * of the boot before as 0 (no "reached fastboot", no "try pending"). */
void boot_trace_flush(void)
{
	if (s_ready)
		arch_clean_cache_range((addr_t)TRACE_BASE, TRACE_SIZE);
}

static void flush_hdr(void)
{
	arch_clean_cache_range((addr_t)s_hdr, sizeof(struct trace_hdr));
}

void boot_trace_mark(unsigned int flag)
{
	if (s_ready) {
		s_hdr->flags |= flag;
		flush_hdr();
	}
	if (flag & TF_POWEROFF)
		state_progress(PROGRESS_POWEROFF);
	else if (flag & TF_KERNEL)
		state_progress(PROGRESS_KERNEL);
	else if (flag & TF_FASTBOOT)
		state_progress(PROGRESS_FASTBOOT);
}

/* A fastboot command is at most 64 bytes, and a longer one wedges the link: longer kernel
 * arguments are queued in pieces with oem tryarg:<text>, and a plain oem tryboot uses them. */
static char s_queued_args[192];

int boot_trace_queue_arg(const char *text)
{
	unsigned int n = strlen(s_queued_args), k = strlen(text);

	if (n + (n ? 1 : 0) + k >= sizeof(s_queued_args))
		return -1;
	if (n)
		s_queued_args[n++] = ' ';
	memcpy(s_queued_args + n, text, k + 1);
	return 0;
}

const char *boot_trace_queued_args(void)
{
	return s_queued_args;
}

void boot_trace_request_try(const char *extra_args)
{
	unsigned int i = 0;

	if (!s_ready)
		return;
	if ((!extra_args || !extra_args[0]) && s_queued_args[0])
		extra_args = s_queued_args;
	if (extra_args)
		for (; extra_args[i] && i < sizeof(s_hdr->extra_args) - 1; i++)
			s_hdr->extra_args[i] = extra_args[i];
	s_hdr->extra_args[i] = '\0';
	s_hdr->flags |= TF_TRY_PENDING;
	flush_hdr();
	if (s_state_ok) {
		memset(s_state.extra_args, 0, sizeof(s_state.extra_args));
		for (i = 0; i < sizeof(s_state.extra_args) - 1 && s_hdr->extra_args[i]; i++)
			s_state.extra_args[i] = s_hdr->extra_args[i];
		s_state.try_pending = 1;
		state_write();
	}
}

/* the extra kernel arguments of this boot attempt, "" for none */
const char *boot_trace_extra_args(void)
{
	if (s_state_ok)
		return s_is_try ? s_try_args : "";
	if (!s_ready || !(s_hdr->flags & TF_TRY))
		return "";
	return (const char *)s_hdr->extra_args;
}

/* Emit text as fastboot INFO lines of at most `width` characters, split at newlines. */
static void emit_text(const char *p, unsigned int n, void (*info)(const char *), unsigned int width)
{
	char line[64];
	unsigned int i, k = 0;

	for (i = 0; i < n; i++) {
		char c = p[i];

		if (c == '\r')
			continue;
		if (c == '\n' || k == width) {
			line[k] = '\0';
			info(line);
			k = 0;
			if (c == '\n')
				continue;
		}
		line[k++] = (c >= 32 && c < 127) ? c : '.';
	}
	if (k) {
		line[k] = '\0';
		info(line);
	}
}

void boot_trace_dump(void (*info)(const char *))
{
	char s[64];

	if (!s_have_last) {
		info("no trace of the previous boot");
		return;
	}
	snprintf(s, sizeof(s), "boot %u flags 0x%x stage %u at %u ms, %u%s bytes",
		 s_last_hdr.seq, s_last_hdr.flags, s_last_hdr.stage, s_last_hdr.stage_ms,
		 s_last_hdr.wrapped ? (unsigned int)RING_SIZE : s_last_hdr.head,
		 s_last_hdr.wrapped ? " (wrapped)" : "");
	info(s);
	if (s_last_hdr.wrapped)
		emit_text(s_last + s_last_hdr.head, RING_SIZE - s_last_hdr.head, info, 58);
	emit_text(s_last, s_last_hdr.head, info, 58);
}

/* The kernel console ramoops kept (persistent_ram_buffer: signature, start, size, then data). */
void boot_trace_dump_kmsg(void (*info)(const char *))
{
	static int mapped;
	const volatile unsigned int *h = (const volatile unsigned int *)KMSG_BASE;
	const char *data = (const char *)(KMSG_BASE + 12);
	unsigned int start, size, cap = KMSG_SIZE - 12;

	if (!mapped) {
		if (arch_mmu_map((uint64_t)KMSG_BASE, (vaddr_t)KMSG_BASE,
				 MMU_MEMORY_TYPE_NORMAL | MMU_MEMORY_AP_P_RW_U_NA, KMSG_SIZE) != 0) {
			info("cannot map the ramoops console");
			return;
		}
		mapped = 1;
	}
	if (h[0] != RAMOOPS_SIG) {
		info("no kernel console in ramoops");
		return;
	}
	start = h[1];
	size = h[2];
	if (start > cap || size > cap) {
		info("ramoops console header out of range");
		return;
	}
	if (size == cap) {   /* full: the oldest text starts at start */
		emit_text(data + start, cap - start, info, 58);
		emit_text(data, start, info, 58);
	} else {
		emit_text(data, size, info, 58);
	}
}

/* ---- the guard: a hang that keeps the hardware watchdog fed (or disables it) is still ended ----
 * A one shot timer started when the platform set up begins. If the boot has not reached
 * fastboot or the kernel when it fires, the board is reset by the watchdog's software reset,
 * which keeps the RAM, so the next boot finds this boot's log and offers rescue fastboot. A hang
 * with the interrupts off is left to the hardware watchdog. */
static timer_t s_guard;
static int s_guard_on;

static enum handler_return guard_fire(struct timer *t, time_t now, void *arg)
{
	extern void mtk_arch_reset(char mode);

	(void)t; (void)now; (void)arg;
	if (!s_guard_on)
		return INT_NO_RESCHEDULE;
	dprintf(CRITICAL, "[TRACE] guard: no fastboot or kernel after %u ms, warm reset\n",
		(unsigned int)(unsigned long)arg);
	mtk_arch_reset(1);
	return INT_NO_RESCHEDULE;
}

void boot_trace_guard_start(unsigned int ms)
{
	timer_initialize(&s_guard);
	s_guard_on = 1;
	timer_set_oneshot(&s_guard, ms, guard_fire, (void *)(unsigned long)ms);
	dprintf(CRITICAL, "[TRACE] guard armed, %u ms\n", ms);
}

void boot_trace_guard_stop(void)
{
	s_guard_on = 0;
	timer_cancel(&s_guard);
}

/* ---- the previous boot's log on the eMMC, for a board whose USB is not up yet ----
 * Written 256 KB into misc (512 KB): Android and the preloader use only its first 64 KB (the
 * recovery message, the A/B record at 2048, the virtual A/B state), so the rest is free. Never a
 * partition of the other slot: an OTA fills those and a later save would break that slot. A
 * 512-byte text header, then the log oldest first, 0x10200 bytes in all. SP Flash Tool's readback
 * of 0x48000 (misc starts at 0x8000), length 0x10200, brings it to the PC. */
#define SAVE_PART    "misc"
#define SAVE_OFFSET  0x40000u
#define SAVE_HDR     512u

static char s_save_buf[(SAVE_HDR + RING_SIZE + 511u) & ~511u] __attribute__((aligned(64)));

int boot_trace_save_last(void)
{
	char *buf = s_save_buf;
	unsigned int n = 0, len;

	if (!s_have_last) {
		dprintf(CRITICAL, "[TRACE] the previous boot left no log in RAM\n");
		return -1;
	}
	memset(buf, 0, sizeof(s_save_buf));
	{
		extern BOOT_ARGUMENT *g_boot_arg;
		snprintf(buf, SAVE_HDR, "GAMMAOS LK TRACE\nboot %u flags 0x%x stage %u at %u ms, %u%s bytes\n"
			 "this boot %u: reset status 0x%x, preloader boot reason %u\n",
			 s_last_hdr.seq, s_last_hdr.flags, s_last_hdr.stage, s_last_hdr.stage_ms,
			 s_last_hdr.wrapped ? (unsigned int)RING_SIZE : s_last_hdr.head,
			 s_last_hdr.wrapped ? " (wrapped)" : "",
			 s_hdr->seq, s_hdr->reserved[0], g_boot_arg ? g_boot_arg->boot_reason : 0xffffffffu);
	}
	if (s_last_hdr.wrapped) {
		memcpy(buf + SAVE_HDR, s_last + s_last_hdr.head, RING_SIZE - s_last_hdr.head);
		n = RING_SIZE - s_last_hdr.head;
	}
	memcpy(buf + SAVE_HDR + n, s_last, s_last_hdr.head);
	len = sizeof(s_save_buf);   /* whole 512-byte blocks */
	if (partition_write(SAVE_PART, SAVE_OFFSET, (u8 *)buf, len) != (ssize_t)len) {
		dprintf(CRITICAL, "[TRACE] saving the last log to %s failed\n", SAVE_PART);
		return -1;
	}
	dprintf(CRITICAL, "[TRACE] last log saved to %s (%u bytes)\n", SAVE_PART, len);
	return 0;
}

/* ---- this boot's log so far, on the eMMC, at milestones of a boot attempt ----
 * The RAM trace is lost when a hang ends in a power key reset (DRAM is not kept) and once a
 * kernel runs (the ring is its pmsg zone). A boot attempt therefore also copies its log to misc
 * at +0x60000 (SP Flash readback 0x68000, length 0x10200), clear of the copy of the previous boot
 * at +0x40000 and inside the partition's 512 KB, each time it passes a milestone; `oem
 * savedtrace` prints the copy, whatever reset came after. */
#define NOW_OFFSET  0x60000u

static int s_force_save;

/* 1 unless the preloader says this boot came from a long power key press. The TOPRGU status
 * register is no use here: the preloader reads and clears it before the LK runs. */
static int boot_after_reset(void)
{
	extern BOOT_ARGUMENT *g_boot_arg;
	unsigned int r = g_boot_arg ? g_boot_arg->boot_reason : 0;

	/* everything but the long power key press that brings a stuck board back: what a warm
	 * restart reports varies, the copy must survive the press that ends it */
	return r != BR_LONG_POWKEY;
}

/* oem savenow: copy this boot's log to misc whatever the reset reason, and say what this boot
 * saw, to check the copy path and the reset detection from fastboot */
void boot_trace_force_save(void (*info)(const char *))
{
	extern BOOT_ARGUMENT *g_boot_arg;
	extern void boot_trace_save_now(const char *milestone);
	char s[64];

	snprintf(s, sizeof(s), "reset status 0x%x, preloader boot reason %u, log of the previous boot %s",
		 s_ready ? s_hdr->reserved[0] : 0, g_boot_arg ? g_boot_arg->boot_reason : 0xffffffffu,
		 s_have_last ? "kept" : "lost");
	info(s);
	snprintf(s, sizeof(s), "init saw seq %u flags 0x%x; now seq %u flags 0x%x",
		 s_last_hdr.seq, s_last_hdr.flags, s_hdr->seq, s_hdr->flags);
	info(s);
	snprintf(s, sizeof(s), "state ok %d magic 0x%x try %u progress %u seq %u, prev %u",
		 s_state_ok, s_state.magic, s_state.try_pending, s_state.progress, s_state.seq, s_prev_progress);
	info(s);
	s_force_save = 1;
	boot_trace_save_now("oem savenow");
	s_force_save = 0;
}

void boot_trace_save_now(const char *milestone)
{
	extern BOOT_ARGUMENT *g_boot_arg;
	char *buf = s_save_buf;
	unsigned int n = 0, head, wrapped;

	/* Boot attempts only (the boot state on the eMMC says which): the boot that comes back
	 * after one must not write over its copy. */
	if (!s_ready || (!s_force_save && !boot_trace_is_try()))
		return;
	memset(buf, 0, sizeof(s_save_buf));
	head = s_hdr->head;
	wrapped = s_hdr->wrapped;
	snprintf(buf, SAVE_HDR, "GAMMAOS LK TRACE NOW\nboot %u flags 0x%x reset status 0x%x boot reason %u, "
		 "previous log %s, milestone %s, %u%s bytes\n",
		 s_hdr->seq, s_hdr->flags, s_hdr->reserved[0],
		 g_boot_arg ? g_boot_arg->boot_reason : 0xffffffffu, s_have_last ? "kept" : "lost", milestone,
		 wrapped ? (unsigned int)RING_SIZE : head, wrapped ? " (wrapped)" : "");
	if (wrapped) {
		memcpy(buf + SAVE_HDR, (const void *)(s_ring + head), RING_SIZE - head);
		n = RING_SIZE - head;
	}
	memcpy(buf + SAVE_HDR + n, (const void *)s_ring, head);
	if (partition_write(SAVE_PART, NOW_OFFSET, (u8 *)buf, sizeof(s_save_buf)) != (ssize_t)sizeof(s_save_buf))
		dprintf(CRITICAL, "[TRACE] saving the log at %s failed\n", milestone);
	else
		dprintf(CRITICAL, "[TRACE] log saved at %s\n", milestone);
}

/* the copy boot_trace_save_now() left in misc */
void boot_trace_dump_saved(void (*info)(const char *))
{
	char *buf = s_save_buf;
	unsigned int len;

	if (partition_read(SAVE_PART, NOW_OFFSET, (u8 *)buf, sizeof(s_save_buf)) != (ssize_t)sizeof(s_save_buf)) {
		info("cannot read the saved log");
		return;
	}
	if (memcmp(buf, "GAMMAOS LK TRACE NOW", 20) != 0) {
		info("no saved log");
		return;
	}
	for (len = sizeof(s_save_buf); len > SAVE_HDR && buf[len - 1] == '\0'; len--)
		;
	emit_text(buf, strnlen(buf, SAVE_HDR), info, 58);
	emit_text(buf + SAVE_HDR, len - SAVE_HDR, info, 58);
}

/* ---- the device tree handed to the kernel, for a comparison with the stock one ----
 * A boot attempt writes its final FDT 16 MB into expdb (20 MB, the AEE exception log, which only
 * uses its start); `oem fdtdump` prints it in hex for dtc on the PC. */
#define FDT_PART    "expdb"
#define FDT_OFFSET  0x1000000u
#define FDT_MAGIC   0x54444647u   /* "GFDT" */
#define FDT_MAX     0x80000u

void boot_trace_save_fdt(const void *fdt, unsigned int size)
{
	static unsigned int hdr[128] __attribute__((aligned(64)));
	unsigned int len = (size + 511u) & ~511u;

	if (!boot_trace_is_try() || size == 0 || len > FDT_MAX)
		return;
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = FDT_MAGIC;
	hdr[1] = size;
	hdr[2] = s_state_ok ? s_state.seq : 0;
	if (partition_write(FDT_PART, FDT_OFFSET, (u8 *)hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
	    partition_write(FDT_PART, FDT_OFFSET + 512u, (u8 *)fdt, len) != (ssize_t)len)
		dprintf(CRITICAL, "[TRACE] saving the device tree failed\n");
	else
		dprintf(CRITICAL, "[TRACE] device tree saved (%u bytes)\n", size);
}

void boot_trace_dump_fdt(void (*info)(const char *))
{
	static const char hexd[] = "0123456789abcdef";
	unsigned int hdr[128] __attribute__((aligned(64)));
	unsigned char *buf;
	unsigned int size, i, k;
	char line[64];

	if (partition_read(FDT_PART, FDT_OFFSET, (u8 *)hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
	    hdr[0] != FDT_MAGIC || hdr[1] == 0 || hdr[1] > FDT_MAX) {
		info("no saved device tree");
		return;
	}
	size = hdr[1];
	buf = (unsigned char *)s_save_buf;   /* reused in 0x10200-byte windows */
	snprintf(line, sizeof(line), "FDT %u bytes, boot %u", size, hdr[2]);
	info(line);
	for (i = 0; i < size; i += sizeof(s_save_buf) - 512u) {
		unsigned int chunk = size - i, j;

		if (chunk > sizeof(s_save_buf) - 512u)
			chunk = sizeof(s_save_buf) - 512u;
		if (partition_read(FDT_PART, FDT_OFFSET + 512u + i, buf, (chunk + 511u) & ~511u) < 0) {
			info("read failed");
			return;
		}
		for (j = 0; j < chunk; j += 28) {
			unsigned int n = chunk - j < 28 ? chunk - j : 28;

			for (k = 0; k < n; k++) {
				line[2 * k] = hexd[buf[j + k] >> 4];
				line[2 * k + 1] = hexd[buf[j + k] & 15];
			}
			line[2 * n] = '\0';
			info(line);
		}
	}
}

/* ---- every ramoops zone the kernel left in pstore ----
 * The kernel lays its zones out from its own settings (oops and panic records, the console,
 * ftrace, pmsg); each starts with a persistent_ram_buffer header: signature, start, size. The
 * whole carve-out is scanned for headers, page by page, and every zone with text is printed. */
#define PSTORE_BASE  0x4D010000u
#define PSTORE_SIZE  0xE0000u

void boot_trace_dump_pstore(void (*info)(const char *))
{
	static int mapped;
	unsigned int off, found = 0;
	char s[64];

	if (!mapped) {
		if (arch_mmu_map((uint64_t)PSTORE_BASE, (vaddr_t)PSTORE_BASE,
				 MMU_MEMORY_TYPE_NORMAL | MMU_MEMORY_AP_P_RW_U_NA, PSTORE_SIZE) != 0) {
			info("cannot map pstore");
			return;
		}
		mapped = 1;
	}
	for (off = 0; off < PSTORE_SIZE; off += 0x1000) {
		const volatile unsigned int *h = (const volatile unsigned int *)(PSTORE_BASE + off);
		unsigned int start, size, cap = PSTORE_SIZE - off - 12;
		const char *data = (const char *)(PSTORE_BASE + off + 12);

		if (h[0] != RAMOOPS_SIG)
			continue;
		start = h[1];
		size = h[2];
		found++;
		snprintf(s, sizeof(s), "== zone at +0x%x: start 0x%x size 0x%x", off, start, size);
		info(s);
		if (size == 0 || size > cap || start > cap)
			continue;
		if (start > 0 && start < size) {   /* a ring that wrapped: oldest text from start */
			emit_text(data + start, size - start, info, 58);
			emit_text(data, start, info, 58);
		} else {
			emit_text(data, size, info, 58);
		}
	}
	if (!found)
		info("no ramoops zone in pstore");
}
