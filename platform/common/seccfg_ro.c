/*
 * seccfg_ro.c - the security configuration partition read without the prebuilt security
 * library (SECCFG_READ_ONLY builds).
 *
 * The prebuilt library's seccfg code predates the Android 12 preloaders: it checks the record
 * with its own hardware keyed hash and, when that fails, writes a fresh record of its own. Here
 * the record is only read, never written. Layout (both the main record at 0 and its backup at
 * 0x3c): magic 0x4d4d4d4d, version, size 0x3c, lock state, critical lock state, secure boot
 * runtime state, end marker 0x45454545, then a hash keyed with the SoC's hardware key, which the
 * LK cannot check and does not need: the lock state is what Android and the TEE are told, and a
 * board keeps it as the stock bootloader left it. Lock state 3 is unlocked, anything else locked
 * (the rule the Android 12 LK applies).
 *
 * This file stands in for every seccfg entry point the rest of the LK and the library use, so the
 * library's own seccfg code is never linked. The calls that would change the record fail.
 */
#include <string.h>
#include <debug.h>
#include <part_interface.h>

#define SECCFG_MAGIC     0x4d4d4d4du
#define SECCFG_END       0x45454545u
#define SECCFG_REC_SIZE  0x3c
#define LOCK_UNLOCKED    3
#define LOCK_DEFAULT     1             /* what the stock code assumes for an unreadable record */

struct seccfg_rec {
	unsigned int magic, version, size, lock_state, critical_lock_state, sboot_runtime, end;
	unsigned char hash[32];
} __attribute__((packed));

static int s_loaded;
static unsigned int s_lock = LOCK_DEFAULT;

static int rec_ok(const struct seccfg_rec *r)
{
	return r->magic == SECCFG_MAGIC && r->end == SECCFG_END && r->size == SECCFG_REC_SIZE;
}

static void seccfg_load(void)
{
	static unsigned char buf[2 * SECCFG_REC_SIZE];
	const struct seccfg_rec *main_rec = (const struct seccfg_rec *)buf;
	const struct seccfg_rec *backup = (const struct seccfg_rec *)(buf + SECCFG_REC_SIZE);

	if (s_loaded)
		return;
	s_loaded = 1;
	if (partition_read("seccfg", 0, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		dprintf(CRITICAL, "[seccfg] read failed, lock state taken as %u\n", s_lock);
		return;
	}
	if (rec_ok(main_rec))
		s_lock = main_rec->lock_state;
	else if (rec_ok(backup))
		s_lock = backup->lock_state;
	dprintf(CRITICAL, "[seccfg] version %u, lock state %u (%s), main %s, backup %s\n",
		main_rec->version, s_lock, s_lock == LOCK_UNLOCKED ? "unlocked" : "locked",
		rec_ok(main_rec) ? "ok" : "bad", rec_ok(backup) ? "ok" : "bad");
}

/* ---- the entry points ---- */

unsigned int get_lock_state(unsigned int *lock_state)
{
	seccfg_load();
	*lock_state = s_lock;
	return 0;
}

unsigned int seccfg_get_lock_state(unsigned int *lock_state)
{
	return get_lock_state(lock_state);
}

unsigned int seccfg_set_lock_state(unsigned int lock_state)
{
	dprintf(CRITICAL, "[seccfg] lock state change to %u refused (read only)\n", lock_state);
	return 1;
}

unsigned int seccfg_get_dm_verity_state(unsigned int *state)
{
	*state = 0;                    /* no hash tree corruption recorded */
	return 0;
}

unsigned int seccfg_set_dm_verity_state(unsigned int state)
{
	dprintf(CRITICAL, "[seccfg] dm-verity state %u not recorded (read only)\n", state);
	return 1;
}

/* the library's own uses (sec_boot.o) */
unsigned int sec_get_seccfg(void)
{
	seccfg_load();
	return 0;
}

unsigned int sec_backup(void)
{
	return 0;
}

unsigned int sec_init_seccfg_major_NV(void)
{
	return 0;
}

void sec_show_status(void)
{
	seccfg_load();
}
