/* Host test for exFAT detection in fat_mount: a bare exFAT VBR and an MBR whose
 * partition type 0x07 points at an exFAT VBR must both return -4 (distinct from
 * the generic -3), so the caller can tell the user to reformat FAT32. Crafted in
 * memory (no mkfs.exfat needed): exFAT VBR = jump byte + "EXFAT   " at offset 3
 * + the 0x55AA boot signature. */
#include <stdio.h>
#include <string.h>
#include "fat_ro.h"

static unsigned char disk[4 * 512];

static unsigned rd(void *c, uint32_t l, uint32_t n, void *b)
{
	(void)c;
	if ((l + n) * 512 > sizeof disk) return 0;
	memcpy(b, disk + l * 512, n * 512);
	return n;
}

static void put_exfat_vbr(unsigned char *s)
{
	memset(s, 0, 512);
	s[0] = 0xEB; s[1] = 0x76; s[2] = 0x90;      /* exFAT jump */
	memcpy(s + 3, "EXFAT   ", 8);
	s[510] = 0x55; s[511] = 0xAA;
}

int main(void)
{
	fat_vol v;
	int fails = 0, rc;

	/* case 1: bare exFAT superfloppy at LBA 0 */
	memset(disk, 0, sizeof disk);
	put_exfat_vbr(disk);
	rc = fat_mount(&v, rd, 0);
	printf("bare exFAT: rc=%d (want -4)\n", rc);
	if (rc != -4) { printf("  FAIL\n"); fails++; }

	/* case 2: MBR with a type-0x07 partition at LBA 1 that is exFAT */
	memset(disk, 0, sizeof disk);
	disk[510] = 0x55; disk[511] = 0xAA;         /* MBR signature, jump byte 0 (not a VBR) */
	{
		unsigned char *e = disk + 0x1BE;        /* first partition entry */
		e[4] = 0x07;                            /* type = exFAT/NTFS */
		e[8] = 1; e[9] = 0; e[10] = 0; e[11] = 0;   /* start LBA = 1 */
	}
	put_exfat_vbr(disk + 512);                  /* the partition's VBR */
	rc = fat_mount(&v, rd, 0);
	printf("MBR exFAT: rc=%d (want -4)\n", rc);
	if (rc != -4) { printf("  FAIL\n"); fails++; }

	/* case 3: type-0x07 partition that is NOT exFAT must not false-positive as -4 */
	memset(disk, 0, sizeof disk);
	disk[510] = 0x55; disk[511] = 0xAA;
	{
		unsigned char *e = disk + 0x1BE;
		e[4] = 0x07; e[8] = 1;
	}
	memset(disk + 512, 0, 512);                 /* garbage VBR, no EXFAT sig */
	disk[512 + 510] = 0x55; disk[512 + 511] = 0xAA;
	rc = fat_mount(&v, rd, 0);
	printf("MBR 0x07 non-exFAT: rc=%d (want -3)\n", rc);
	if (rc != -3) { printf("  FAIL: false exFAT positive\n"); fails++; }

	printf(fails ? "FAT_EXFAT TEST: %d FAIL\n" : "FAT_EXFAT TEST: PASS\n", fails);
	return fails ? 1 : 0;
}
