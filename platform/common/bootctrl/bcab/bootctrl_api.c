/*
 * bootctrl_api.c - A/B slot metadata in the AOSP bootloader_control format ("BCAB"), the one the
 * Android 12 preloaders and the bootctrl HAL of these boards keep in misc at offset 2048
 * (MTK_BOOTCTRL_VERSION = bcab). Same entry points as the other bootctrl versions.
 *
 * Record (32 bytes, little endian): slot_suffix[4], magic 0x42414342, version 1, then bit fields
 * nb_slot:3 recovery_tries_remaining:3 merge_status:3, one reserved byte, slot_info[4] at +12
 * (each two bytes: priority:4 tries_remaining:3 successful_boot:1, verity_corrupted:1
 * reserved:7), eight reserved bytes and a CRC-32 of the 28 bytes before it at +28.
 *
 * The preloader picks the slot and takes a try off it on every boot that Android has not marked
 * successful; the LK gives that try back when it boots the system itself
 * (increase_tries_remaining), and fastboot set_active resets a slot. Nothing else writes the
 * record. A record that is not valid is replaced by a fresh one for slot a.
 */
#include <string.h>
#include <pal_log.h>
#include <part_interface.h>
#include <bootctrl.h>

#define BCAB_PART          "misc"
#define BCAB_OFFSET        2048
#define BCAB_MAGIC         0x42414342u
#define BCAB_VERSION       1
#define BCAB_SLOTS         2
#define BCAB_MAX_PRIORITY  15
#define BCAB_MAX_TRIES     7

struct bcab_record {
	char slot_suffix[4];
	uint32_t magic;
	uint8_t version;
	uint8_t fields0;          /* nb_slot bits 0-2, recovery_tries_remaining bits 3-5, merge_status bits 6-7 */
	uint8_t fields1;          /* merge_status bit 2 in bit 0 */
	uint8_t reserved0;
	uint8_t slot[4][2];       /* [0]: priority bits 0-3, tries bits 4-6, successful bit 7; [1]: verity bit 0 */
	uint8_t reserved1[8];
	uint32_t crc32_le;
} __attribute__((packed));

static const char *const s_suffix[BCAB_SLOTS] = { "_a", "_b" };
static struct bcab_record s_rec;
static int s_loaded;

/* the standard CRC-32 (reflected, polynomial 0xedb88320) */
static uint32_t bcab_crc32(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xffffffffu;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
	}
	return ~c;
}

static unsigned int prio(int s)       { return s_rec.slot[s][0] & 0x0f; }
static unsigned int tries(int s)      { return (s_rec.slot[s][0] >> 4) & 0x07; }
static unsigned int successful(int s) { return (s_rec.slot[s][0] >> 7) & 0x01; }

static void set_slot(int s, unsigned int p, unsigned int t, unsigned int ok)
{
	s_rec.slot[s][0] = (uint8_t)((p & 0x0f) | ((t & 0x07) << 4) | ((ok & 1u) << 7));
}

static int bcab_write(void)
{
	s_rec.crc32_le = bcab_crc32((const uint8_t *)&s_rec, sizeof(s_rec) - sizeof(uint32_t));
	if (partition_write(BCAB_PART, BCAB_OFFSET, (u8 *)&s_rec, sizeof(s_rec)) != (ssize_t)sizeof(s_rec)) {
		pal_log_err("[bootctrl] misc write failed\n");
		return -1;
	}
	return 0;
}

static int bcab_valid(void)
{
	return s_rec.magic == BCAB_MAGIC && s_rec.version == BCAB_VERSION &&
	       s_rec.crc32_le == bcab_crc32((const uint8_t *)&s_rec, sizeof(s_rec) - sizeof(uint32_t));
}

/* a fresh record: slot a active with every try, slot b not bootable */
static void bcab_defaults(void)
{
	memset(&s_rec, 0, sizeof(s_rec));
	memcpy(s_rec.slot_suffix, "_a", 3);
	s_rec.magic = BCAB_MAGIC;
	s_rec.version = BCAB_VERSION;
	s_rec.fields0 = BCAB_SLOTS & 0x07;
	set_slot(0, BCAB_MAX_PRIORITY, BCAB_MAX_TRIES, 0);
	set_slot(1, 0, 0, 0);
}

static int bcab_load(void)
{
	if (s_loaded)
		return 0;
	if (partition_read(BCAB_PART, BCAB_OFFSET, (u8 *)&s_rec, sizeof(s_rec)) != (ssize_t)sizeof(s_rec)) {
		pal_log_err("[bootctrl] misc read failed\n");
		return -1;
	}
	s_loaded = 1;
	if (!bcab_valid()) {
		pal_log_err("[bootctrl] no valid A/B record in misc (magic 0x%x): writing a fresh one for slot a\n",
			    s_rec.magic);
		bcab_defaults();
		bcab_write();
	}
	pal_log_err("[bootctrl] a: priority %u tries %u ok %u, b: priority %u tries %u ok %u\n",
		    prio(0), tries(0), successful(0), prio(1), tries(1), successful(1));
	return 0;
}

static int slot_of(const char *suffix)
{
	int s;

	if (!suffix)
		return -1;
	for (s = 0; s < BCAB_SLOTS; s++)
		if (!strcmp(suffix, s_suffix[s]))
			return s;
	return -1;
}

static int bootable(int s)
{
	return prio(s) > 0 && (tries(s) > 0 || successful(s));
}

/* the slot the preloader boots: the bootable one with the higher priority, slot a on a tie */
static int active_slot(void)
{
	if (bcab_load() != 0)
		return 0;
	if (bootable(1) && (!bootable(0) || prio(1) > prio(0)))
		return 1;
	return 0;
}

const char *get_suffix(void)
{
	return s_suffix[active_slot()];
}

int set_active_slot(const char *suffix)
{
	int s = slot_of(suffix), other;

	if (s < 0 || bcab_load() != 0)
		return -1;
	other = !s;
	set_slot(s, BCAB_MAX_PRIORITY, BCAB_MAX_TRIES, 0);
	if (prio(other) >= BCAB_MAX_PRIORITY)
		set_slot(other, BCAB_MAX_PRIORITY - 1, tries(other), successful(other));
	memcpy(s_rec.slot_suffix, s_suffix[s], 3);
	return bcab_write();
}

uint8_t get_retry_count(const char *suffix)
{
	int s = slot_of(suffix);

	if (s < 0 || bcab_load() != 0)
		return 0;
	return (uint8_t)tries(s);
}

int get_bootup_status(const char *suffix)
{
	int s = slot_of(suffix);

	if (s < 0 || bcab_load() != 0)
		return 0;
	return (int)successful(s);
}

int get_bootable_status(const char *suffix)
{
	int s = slot_of(suffix);

	if (s < 0 || bcab_load() != 0)
		return 0;
	return bootable(s);
}

/* The system is about to boot from the active slot: give back the try the preloader took for
 * this boot, so only boots that never reach Android count against the slot. */
int increase_tries_remaining(void)
{
	int s = active_slot();

	if (successful(s) || tries(s) == 0 || tries(s) >= BCAB_MAX_TRIES)
		return 0;
	set_slot(s, prio(s), tries(s) + 1, 0);
	return bcab_write();
}
