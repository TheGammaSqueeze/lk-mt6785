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

	/* Safe (read-only) PMIC LDO dump so we can see which rails are powered at LK
	 * time BEFORE the msdc1 bring-up. VEMC feeds the eMMC (boots) so it must read
	 * enabled = a known-good reference; the external SD rail should be identifiable
	 * by comparison. bit0 of each *_CON0 is the SW enable. Raw MT6359 offsets. */
	{
		extern unsigned msdc_pmic_read(unsigned reg);
		static const struct { const char *nm; unsigned reg; } ldo[] = {
			{ "VEMC(eMMC)", 0x1cb8 }, { "VSIM1", 0x1cc8 }, { "VSIM2", 0x1cd8 },
			{ "VIO18", 0x1ca8 }, { "VIO28", 0x1d5a }, { "VUSB", 0x1d08 },
		};
		int k;
		for (k = 0; k < (int)(sizeof ldo / sizeof ldo[0]); k++) {
			unsigned rv = msdc_pmic_read(ldo[k].reg);
			snprintf(lbuf, sizeof lbuf, "sd: PMIC %-10s CON0[%04x]=0x%04x en=%u",
				 ldo[k].nm, ldo[k].reg, rv, rv & 1u);
			fastboot_info(lbuf);
		}
	}

	if (gba_sd_hw_init() != 0) {
		snprintf(lbuf, sizeof lbuf, "sd: msdc1 init FAILED, mmc rc=%d (no card / power / pinmux)", gba_sd_hw_rc());
		fastboot_fail(lbuf);
		return;
	}
	fastboot_info("sd: msdc1 (external microSD) init OK");
	n = gba_sd_bread(0, 1, s0);
	snprintf(lbuf, sizeof lbuf, "sd: read(dev1,lba0)=%lu sig=%02x%02x b0=%02x",
		 n, s0[510], s0[511], s0[0]);
	fastboot_info(lbuf);
	if (n != 1) { fastboot_fail("sd: sector0 read failed on the microSD"); return; }

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
	if (gba_sd_hw_init() != 0) { fastboot_fail("sd-read: microSD host init failed"); return; }
	n = gba_sd_bread((uint32_t)lba, 1, sec);
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
