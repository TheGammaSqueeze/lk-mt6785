/*
 * block_fdt_parity.c - what the Konkr Pocket Block's own LK hands the kernel and this tree did
 * not: the device tree and command line differences found by comparing the two LKs' live
 * device trees (/sys/firmware/fdt) and /proc/cmdline on the same unit.
 *
 * - atag,masp is built here from the preloader's ROM_INFO version 2 block (found in the
 *   preloader partition) and the security boot tag, instead of the prebuilt security library,
 *   which reads the version 1 layout and so got the USB download attribute (0x11, not 0x22) and
 *   the crypto seed (a piece of the project name) wrong.
 * - /chosen gains atag,chipid, aee,enable, the mrdump,* properties, the dfd,* properties and the
 *   (empty, WiFi only board) ccci,modem_info_v2, and loses tee_reserved_mem, which the stock LK
 *   only writes in /memory.
 * - /bootprof carries the boot times (the stock kernel's bootprof module reads them there).
 * - the secure memory CMA region is 0x11000000 bytes, as the stock LK leaves it.
 */
#include <string.h>
#include <malloc.h>
#include <debug.h>
#include <libfdt.h>
#include <part_interface.h>
#include <platform/mt_reg_base.h>
#include <platform/mt_typedefs.h>
#include <platform/boot_mode.h>
#include <platform/sec_devinfo.h>
#include "../common/avb/libavb/avb_sha.h"

extern BOOT_ARGUMENT *g_boot_arg;
extern unsigned int get_lock_state(unsigned int *lock_state);

#define ATAG_MASP          0x41000866u
#define ATAG_CHIPID_SIZE   6u          /* the stock size field; five words follow it */
#define SEC_TAG_SUSBDL     0x48u       /* boot tag 0x88610035 payload offsets */
#define SEC_TAG_SBOOT      0x4Cu
#define SEC_TAG_LEN_MIN    0x51u

#define ROMINFO_MAGIC      "AND_ROMINFO_v"
#define ROMINFO_SCAN       0xA2000u    /* how far into the preloader partition to look */
#define ROMINFO_CHUNK      0x3000u
#define ROMINFO_V2_LEN     0x78u
#define ROMINFO_VER        0x12u
#define ROMINFO_SUSBDL     0x48u
#define ROMINFO_SBC        0x4Cu
#define ROMINFO_SEED       0x68u
#define SEED_LEN           16u

#define SEC_CTRL           0x11CE0060u /* bit 1: secure boot fused on */
#define SEC_PUBK_HASH      0x11CE0090u /* eight words, 0x10 apart */

#define MRDUMP_CB_ADDR     0x0010F300u /* the preloader's control block in SRAM */
#define MRDUMP_CB_SIZE     0x00000E00u
#define MRDUMP_DDR_RSV_OK  0x9502u
#define DFD_BASE           0x90000000u

/* ---- the security boot tag (0x88610035), kept from the boot argument parse ---- */
static unsigned int s_tag_susbdl, s_tag_sboot;
static int s_tag_seen;

void block_sec_tag_capture(const void *payload, unsigned int len)
{
	const unsigned char *p = payload;

	if (len < SEC_TAG_LEN_MIN)
		return;
	memcpy(&s_tag_susbdl, p + SEC_TAG_SUSBDL, 4);
	memcpy(&s_tag_sboot, p + SEC_TAG_SBOOT, 4);
	s_tag_seen = 1;
}

/* ---- ROM_INFO version 2 from the preloader partition ---- */
static int s_rom_read;
static unsigned int s_rom_sbc = 0x11, s_rom_susbdl = 0x11;
static char s_rom_seed[SEED_LEN];

static void rominfo_read(void)
{
	unsigned char *buf;
	unsigned int off, keep = sizeof(ROMINFO_MAGIC) - 1;

	if (s_rom_read)
		return;
	s_rom_read = 1;
	buf = malloc(ROMINFO_CHUNK + ROMINFO_V2_LEN);
	if (!buf)
		return;
	/* chunks overlap by the length of the block, so one that straddles a boundary is whole */
	for (off = 0; off < ROMINFO_SCAN; off += ROMINFO_CHUNK) {
		unsigned int i;

		if (partition_read("preloader", off, buf, ROMINFO_CHUNK + ROMINFO_V2_LEN) < 0)
			break;
		for (i = 0; i + keep <= ROMINFO_CHUNK; i++) {
			unsigned short ver;

			if (memcmp(buf + i, ROMINFO_MAGIC, keep) != 0)
				continue;
			memcpy(&ver, buf + i + ROMINFO_VER, 2);
			if (ver != 2) {
				dprintf(CRITICAL, "[parity] ROM_INFO version %u, not 2: kept the defaults\n", ver);
				goto done;
			}
			memcpy(&s_rom_susbdl, buf + i + ROMINFO_SUSBDL, 4);
			memcpy(&s_rom_sbc, buf + i + ROMINFO_SBC, 4);
			memcpy(s_rom_seed, buf + i + ROMINFO_SEED, SEED_LEN);
			dprintf(CRITICAL, "[parity] ROM_INFO v2 at 0x%x: sbc 0x%x susbdl 0x%x\n",
				off + i, s_rom_sbc, s_rom_susbdl);
			goto done;
		}
	}
	dprintf(CRITICAL, "[parity] no ROM_INFO in the preloader partition\n");
done:
	free(buf);
}

/* atag,masp: 22 little endian words, the layout the Block's stock LK writes */
unsigned *block_atag_masp(unsigned *ptr)
{
	unsigned int lock = 1, susbdl, sbc, w[4], i;
	AvbSHA256Ctx ctx;
	const uint8_t *digest;

	rominfo_read();
	sbc = s_rom_sbc;
	susbdl = s_rom_susbdl;
	if (s_tag_seen) {	/* the preloader's boot tag wins over its image, as on stock */
		susbdl = s_tag_susbdl;
		sbc = s_tag_sboot;
	}
	if (get_lock_state(&lock) != 0) {
		*ptr++ = 2;
		*ptr++ = ATAG_MASP;
		return ptr;
	}
	lock = (lock != 3);	/* 1 unless seccfg says unlocked (3), as the stock getter gives it */
	*ptr++ = 22;
	*ptr++ = ATAG_MASP;
	*ptr++ = sbc;
	*ptr++ = susbdl;
	*ptr++ = (DRV_Reg32(SEC_CTRL) >> 1) & 1u;
	*ptr++ = lock;

	/* the device id: the first 16 bytes of a SHA-256 over efuse words 12 and 13, twice */
	w[0] = w[2] = get_devinfo_with_index(12);
	w[1] = w[3] = get_devinfo_with_index(13);
	avb_sha256_init(&ctx);
	avb_sha256_update(&ctx, (const uint8_t *)w, sizeof(w));
	digest = avb_sha256_final(&ctx);
	memcpy(ptr, digest, 16);
	ptr += 4;

	memcpy(ptr, s_rom_seed, SEED_LEN);
	ptr += 4;

	for (i = 0; i < 8; i++)
		*ptr++ = DRV_Reg32(SEC_PUBK_HASH + i * 0x10u);
	return ptr;
}

/* ---- /chosen, /bootprof and reserved memory ---- */
static void set_raw(void *fdt, int off, const char *name, const void *v, int len)
{
	int ret = fdt_setprop(fdt, off, name, v, len);

	if (ret)
		dprintf(CRITICAL, "[parity] %s: %d\n", name, ret);
}

int block_fdt_parity(void *fdt, unsigned int pl_t, unsigned int lk_t, unsigned int logo_t)
{
	int chosen, off;

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return chosen;

	{	/* atag,chipid: native words, a size field of 6 and five values after it */
		unsigned int c[5];

		c[0] = ATAG_CHIPID_SIZE;
		c[1] = DRV_Reg32(APHW_CODE);
		c[2] = DRV_Reg32(APHW_SUBCODE);
		c[3] = DRV_Reg32(APHW_VER);
		c[4] = DRV_Reg32(APSW_VER);
		set_raw(fdt, chosen, "atag,chipid", c, sizeof(c));
	}

	/* the crash dump the kernel prepares: minimal mode, the preloader's control block */
	fdt_setprop_string(fdt, chosen, "aee,enable", "mini");
	fdt_setprop_string(fdt, chosen, "mrdump,ddr_rsv",
			   g_boot_arg->ddr_reserve_ready == MRDUMP_DDR_RSV_OK ? "yes" : "no");
	{
		unsigned int cb[2] = { cpu_to_fdt32(MRDUMP_CB_ADDR), cpu_to_fdt32(MRDUMP_CB_SIZE) };

		set_raw(fdt, chosen, "mrdump,cblock", cb, sizeof(cb));
	}
	if (g_boot_arg->ddr_reserve_ready == MRDUMP_DDR_RSV_OK)
		fdt_setprop_string(fdt, chosen, "mrdump,lk", "MRDUMP11");

	{	/* the debug (DFD) buffer, at the fixed address the stock LK gives */
		unsigned int base[2] = { 0, cpu_to_fdt32(DFD_BASE) };

		set_raw(fdt, chosen, "dfd,base_addr", base, sizeof(base));
		fdt_setprop_cell(fdt, chosen, "dfd,base_addr_msb", DFD_BASE);
		fdt_setprop_cell(fdt, chosen, "dfd,cache_dump_support", 1);
	}

	{	/* no modem on the Block: the modem description is there, all zero */
		unsigned int md[12];

		memset(md, 0, sizeof(md));
		set_raw(fdt, chosen, "ccci,modem_info_v2", md, sizeof(md));
		off = fdt_path_offset(fdt, "/mddriver");
		if (off >= 0)
			set_raw(fdt, off, "ccci,modem_info_v2", md, sizeof(md));
	}

	/* the stock LK writes tee_reserved_mem in /memory only, and no reset key mode */
	fdt_delprop(fdt, chosen, "tee_reserved_mem");
	off = fdt_node_offset_by_compatible(fdt, -1, "mediatek, mrdump_ext_rst-eint");
	if (off >= 0)
		fdt_delprop(fdt, off, "force_mode");

	off = fdt_path_offset(fdt, "/bootprof");
	if (off < 0)
		off = fdt_add_subnode(fdt, 0, "bootprof");
	if (off >= 0) {
		fdt_setprop_cell(fdt, off, "pl_t", pl_t);
		fdt_setprop_cell(fdt, off, "lk_t", lk_t);
		fdt_setprop_cell(fdt, off, "lk_logo_t", logo_t);
	}

	off = fdt_path_offset(fdt, "/reserved-memory/ssmr-reserved-cma_memory");
	if (off >= 0) {
		unsigned int size[2] = { 0, cpu_to_fdt32(0x11000000u) };

		set_raw(fdt, off, "size", size, sizeof(size));
	}
	return 0;
}
