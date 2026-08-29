/*
 * Fastboot debug channel for the microSD (AYANEO_GBA_SD). Registers custom oem
 * commands so the SD can be probed straight from the bootloader over USB, without
 * flashing an emu build or reading UART:
 *
 *   fastboot oem sd-probe        - read sector 0, mount FAT, report bios + rom list
 *   fastboot oem sd-read:<lba>   - hex-dump one 512B sector at decimal <lba>
 *   fastboot oem sd-wtest        - write a temp file to /saves/gba, read it back,
 *                                  verify byte-for-byte (validates the write engine
 *                                  on real hardware without playing a game)
 *
 * Results come back as fastboot INFO lines ("(bootloader) ..."). Runs in FASTBOOT
 * mode, where mmc_legacy_init(1) (platform init) has already brought up the card.
 */
#include "fat_ro.h"
#include "sd_fat.h"
#include "fat_wr.h"

extern void fastboot_register(const char *prefix,
			      void (*handle)(const char *arg, void *data, unsigned sz),
			      int allow_locked, int need_download);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern unsigned long mmc_wrap_bread(int dev_num, unsigned long blknr,
				    unsigned long blkcnt, void *dst, unsigned int part_id);
extern int snprintf(char *str, unsigned long size, const char *fmt, ...);

static char lbuf[96];

static unsigned long parse_u(const char *s)
{
	unsigned long v = 0;
	while (*s == ' ' || *s == ':') s++;
	while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned long)(*s - '0'); s++; }
	return v;
}

static void cmd_sd_probe(const char *arg, void *data, unsigned sz)
{
	unsigned char s0[512];
	fat_vol v;
	fat_file f;
	gba_rom_entry roms[16];
	unsigned long n;
	int rc, i, nr;
	(void)arg; (void)data; (void)sz;

	n = mmc_wrap_bread(0, 0, 1, s0, 0);
	snprintf(lbuf, sizeof lbuf, "sd: mmc_wrap_bread(dev0,lba0)=%lu sig=%02x%02x b0=%02x",
		 n, s0[510], s0[511], s0[0]);
	fastboot_info(lbuf);
	if (n != 1) { fastboot_fail("sd: sector0 read failed - card not enumerated at dev0"); return; }

	rc = gba_sd_mount(&v);
	snprintf(lbuf, sizeof lbuf, "sd: fat_mount rc=%d fat32=%d spc=%u clusters=%u",
		 rc, v.is_fat32, v.sec_per_clus, v.total_clusters);
	fastboot_info(lbuf);
	if (rc == -4) { fastboot_fail("sd: exFAT card not supported - reformat as FAT32"); return; }
	if (rc != 0) { fastboot_fail("sd: not a FAT16/32 volume"); return; }

	i = (fat_open(&v, "/gba_bios.bin", &f) == 0);
	snprintf(lbuf, sizeof lbuf, "sd: /gba_bios.bin %s size=%u",
		 i ? "OK" : "MISSING", i ? f.size : 0u);
	fastboot_info(lbuf);

	{
		int tot = 0;
		nr = gba_sd_list_roms(&v, roms, 16, &tot);
		snprintf(lbuf, sizeof lbuf, "sd: /roms/gba count=%d (total=%d)", nr, tot);
		fastboot_info(lbuf);
	}
	for (i = 0; i < nr && i < 8; i++) {
		snprintf(lbuf, sizeof lbuf, "sd: rom[%d] %s (%u)", i, roms[i].name, roms[i].size);
		fastboot_info(lbuf);
	}
	fastboot_okay("sd-probe done");
}

static void cmd_sd_read(const char *arg, void *data, unsigned sz)
{
	unsigned char sec[512];
	unsigned long lba = parse_u(arg);
	unsigned long n;
	int r, c;
	(void)data; (void)sz;
	n = mmc_wrap_bread(0, lba, 1, sec, 0);
	snprintf(lbuf, sizeof lbuf, "sd-read: lba=%lu read=%lu", lba, n);
	fastboot_info(lbuf);
	if (n != 1) { fastboot_fail("sd-read: read failed"); return; }
	for (r = 0; r < 512; r += 16) {           /* 32 hex lines of 16 bytes */
		char *p = lbuf;
		int k = snprintf(lbuf, sizeof lbuf, "%03x:", r);
		p += k;
		for (c = 0; c < 16; c++) {
			static const char *h = "0123456789abcdef";
			unsigned b = sec[r + c];
			*p++ = ' '; *p++ = h[(b >> 4) & 0xf]; *p++ = h[b & 0xf];
		}
		*p = 0;
		fastboot_info(lbuf);
	}
	fastboot_okay("sd-read done");
}

static void cmd_sd_wtest(const char *arg, void *data, unsigned sz)
{
	static unsigned char wbuf[512], rbuf[512];
	fat_vol v;
	fat_file f;
	int rc, i, bad = 0;
	uint32_t got;
	(void)arg; (void)data; (void)sz;

	rc = gba_sd_mount(&v);
	if (rc != 0) { fastboot_fail("sd-wtest: mount failed"); return; }
	if (!v.wr) { fastboot_fail("sd-wtest: no writer attached (BUG)"); return; }

	for (i = 0; i < 512; i++) wbuf[i] = (unsigned char)(i * 7 + 3);
	rc = fat_wr_put(&v, "/saves/gba", "wtest.bin", wbuf, sizeof wbuf);
	snprintf(lbuf, sizeof lbuf, "sd-wtest: fat_wr_put rc=%d", rc);
	fastboot_info(lbuf);
	if (rc != 0) { fastboot_fail("sd-wtest: write failed (is /saves/gba present?)"); return; }

	/* re-mount so nothing is served from a stale cache, then read back */
	if (gba_sd_mount(&v) != 0) { fastboot_fail("sd-wtest: remount failed"); return; }
	if (fat_open(&v, "/saves/gba/wtest.bin", &f) != 0) { fastboot_fail("sd-wtest: readback open failed"); return; }
	got = fat_read(&f, 0, rbuf, sizeof rbuf);
	for (i = 0; i < 512; i++) if (rbuf[i] != wbuf[i]) { bad++; }
	snprintf(lbuf, sizeof lbuf, "sd-wtest: readback size=%u mismatches=%d", got, bad);
	fastboot_info(lbuf);
	if (got != sizeof wbuf || bad) { fastboot_fail("sd-wtest: verify FAILED"); return; }
	fastboot_okay("sd-wtest: write+verify OK");
}

void gba_sd_fastboot_register(void)
{
	fastboot_register("oem sd-probe", cmd_sd_probe, 1, 0);
	fastboot_register("oem sd-read:", cmd_sd_read, 1, 0);
	fastboot_register("oem sd-wtest", cmd_sd_wtest, 1, 0);
}
