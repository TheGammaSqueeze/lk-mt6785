/*
 * bootimg_v3.c - boot header versions 3 and 4 with vendor_boot, and the bootconfig block the
 * kernel reads after the ramdisks (bootimg_v3.h).
 *
 * The layouts are the public Android ones: a boot image is a 4096-byte header page, then the
 * kernel and the generic ramdisk each padded to 4096 (version 4 adds a signature after them).
 * A vendor_boot image is its header padded to page_size, then the vendor ramdisk, the device tree,
 * the ramdisk table (version 4) and the bootconfig section (version 4), each padded to page_size.
 * The bootconfig block the kernel takes is the text of the settings, padded to a multiple of 4
 * with NULs, then its size and the sum of its bytes as two little-endian words, then the
 * "#BOOTCONFIG\n" magic, all placed at the very end of the initrd.
 */
#include <string.h>
#include <pal_log.h>
#include <part_interface.h>
#include <bootimg_v3.h>

#define ALIGN_UP(x, a)       (((x) + (a) - 1) / (a) * (a))
#define BOOTCONFIG_MAGIC     "#BOOTCONFIG\n"
#define BOOTCONFIG_MAGIC_LEN 12
#define ANDROIDBOOT          "androidboot."
#define ANDROIDBOOT_LEN      12

static const uint8_t *s_vb;           /* the vendor_boot image in memory */
static uint32_t s_vb_size;
static char s_cmdline[VENDOR_BOOT_CMDLINE_SIZE + BOOT_V3_CMDLINE_SIZE + 2];

void vendor_boot_set_image(const void *image, uint32_t size)
{
	s_vb = image;
	s_vb_size = size;
}

const void *vendor_boot_image(void)
{
	return s_vb;
}

int bootimg_is_v3(const void *hdr)
{
	const struct boot_v3_hdr *b = hdr;

	return memcmp(b->magic, BOOTIMG_MAGIC, BOOTIMG_MAGIC_SZ) == 0 && b->header_version >= 3;
}

static int vendor_hdr_ok(const struct vendor_boot_hdr *v)
{
	if (memcmp(v->magic, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_SIZE) != 0) {
		pal_log_err("[bootimg] vendor_boot: bad magic\n");
		return 0;
	}
	if (v->header_version < 3 || v->header_version > 4 || v->page_size == 0 ||
	    (v->page_size & (v->page_size - 1)) != 0) {
		pal_log_err("[bootimg] vendor_boot: header version %u page %u not supported\n",
			    v->header_version, v->page_size);
		return 0;
	}
	return 1;
}

/* A string field that may fill its whole array, copied with a terminator. */
static void field_str(char *dst, uint32_t cap, const uint8_t *src, uint32_t len)
{
	uint32_t n = 0;

	while (n < len && n + 1 < cap && src[n])
		n++;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static int fill_legacy(const struct boot_v3_hdr *b, const struct vendor_boot_hdr *v,
		       struct bootimg_hdr *legacy)
{
	char boot_cmd[BOOT_V3_CMDLINE_SIZE + 1];
	uint32_t n;

	if (b->header_version > 4) {
		pal_log_err("[bootimg] boot header version %u not supported\n", b->header_version);
		return -1;
	}
	if (!vendor_hdr_ok(v))
		return -1;
	if (v->header_version != b->header_version)
		pal_log_err("[bootimg] boot header %u with vendor_boot header %u\n",
			    b->header_version, v->header_version);

	field_str(s_cmdline, sizeof(s_cmdline), v->cmdline, VENDOR_BOOT_CMDLINE_SIZE);
	field_str(boot_cmd, sizeof(boot_cmd), b->cmdline, BOOT_V3_CMDLINE_SIZE);
	n = strlen(s_cmdline);
	if (boot_cmd[0] && n + 1 + strlen(boot_cmd) < sizeof(s_cmdline)) {
		if (n)
			s_cmdline[n++] = ' ';
		strcpy(s_cmdline + n, boot_cmd);
	}

	memset(legacy, 0, sizeof(*legacy));
	memcpy(legacy->magic, BOOTIMG_MAGIC, BOOTIMG_MAGIC_SZ);
	legacy->kernel_sz = b->kernel_size;
	legacy->kernel_addr = v->kernel_addr;
	legacy->ramdisk_sz = b->ramdisk_size;          /* the generic ramdisk, inside boot */
	legacy->ramdisk_addr = v->ramdisk_addr;
	legacy->tags_addr = v->tags_addr;
	legacy->page_sz = BOOT_V3_PAGE_SIZE;           /* the boot image's own pages */
	legacy->header_version = b->header_version;
	legacy->os_version = b->os_version;
	legacy->header_size = b->header_size;
	legacy->dtb_size = v->dtb_size;
	legacy->dtb_addr = v->dtb_addr;
	memcpy(legacy->name, v->name, BOOTIMG_NAME_SZ);
	field_str((char *)legacy->cmdline, BOOTIMG_ARGS_SZ, (const uint8_t *)s_cmdline,
		  sizeof(s_cmdline));
	return 0;
}

int bootimg_v3_to_legacy(const void *boot, struct bootimg_hdr *legacy)
{
	if (!s_vb || s_vb_size < sizeof(struct vendor_boot_hdr)) {
		pal_log_err("[bootimg] boot header v3+ without a vendor_boot image\n");
		return -1;
	}
	return fill_legacy(boot, (const struct vendor_boot_hdr *)s_vb, legacy);
}

int bootimg_v3_hdr_from_partitions(const char *boot_part, const char *vendor_part,
				   struct bootimg_hdr *legacy)
{
	static struct boot_v3_hdr b;
	static struct vendor_boot_hdr v;

	if (partition_read(boot_part, 0, (uint8_t *)&b, sizeof(b)) != (int)sizeof(b) ||
	    partition_read(vendor_part, 0, (uint8_t *)&v, sizeof(v)) != (int)sizeof(v)) {
		pal_log_err("[bootimg] cannot read the %s / %s headers\n", boot_part, vendor_part);
		return -1;
	}
	if (!bootimg_is_v3(&b))
		return -1;
	return fill_legacy(&b, &v, legacy);
}

/* Offsets of the vendor_boot sections, -1 size when the image is not usable. */
static const struct vendor_boot_hdr *vb_layout(uint32_t *ramdisk_off, uint32_t *dtb_off,
					       uint32_t *bootconfig_off)
{
	const struct vendor_boot_hdr *v = (const struct vendor_boot_hdr *)s_vb;
	uint32_t pg, off;

	if (!s_vb || s_vb_size < sizeof(*v) || !vendor_hdr_ok(v))
		return NULL;
	pg = v->page_size;
	off = ALIGN_UP(v->header_size, pg);
	*ramdisk_off = off;
	off += ALIGN_UP(v->vendor_ramdisk_size, pg);
	*dtb_off = off;
	off += ALIGN_UP(v->dtb_size, pg);
	if (v->header_version >= 4)
		off += ALIGN_UP(v->ramdisk_table_size, pg);
	*bootconfig_off = off;
	return v;
}

static const void *vb_section(uint32_t off, uint32_t len, uint32_t *size)
{
	if (off > s_vb_size || len > s_vb_size - off) {
		pal_log_err("[bootimg] vendor_boot section at 0x%x (+0x%x) past the image (0x%x)\n",
			    off, len, s_vb_size);
		*size = 0;
		return NULL;
	}
	*size = len;
	return s_vb + off;
}

const void *vendor_boot_ramdisk(uint32_t *size)
{
	uint32_t r, d, c;
	const struct vendor_boot_hdr *v = vb_layout(&r, &d, &c);

	*size = 0;
	return v ? vb_section(r, v->vendor_ramdisk_size, size) : NULL;
}

const void *vendor_boot_dtb(uint32_t *size)
{
	uint32_t r, d, c;
	const struct vendor_boot_hdr *v = vb_layout(&r, &d, &c);

	*size = 0;
	return v ? vb_section(d, v->dtb_size, size) : NULL;
}

const void *vendor_boot_bootconfig(uint32_t *size)
{
	uint32_t r, d, c;
	const struct vendor_boot_hdr *v = vb_layout(&r, &d, &c);

	*size = 0;
	if (!v || v->header_version < 4 || v->bootconfig_size == 0)
		return NULL;
	return vb_section(c, v->bootconfig_size, size);
}

const char *bootimg_v3_cmdline(void)
{
	return s_cmdline;
}

/* ---- bootconfig ---- */

#define BOOT_DEVICES_KEY "androidboot.boot_devices"

/* The end of the command line word starting at s: up to a space outside double quotes. */
static const char *word_end(const char *s)
{
	int quoted = 0;

	for (; *s; s++) {
		if (*s == '"')
			quoted = !quoted;
		else if (*s == ' ' && !quoted)
			break;
	}
	return s;
}

/* 1 when the text block (len bytes) already has a line for the key (klen bytes). */
static int has_key(const char *text, uint32_t len, const char *key, uint32_t klen)
{
	uint32_t i = 0;

	while (i < len) {
		const char *line = text + i;
		uint32_t n = 0;

		while (i + n < len && line[n] != '\n')
			n++;
		if (n > klen && memcmp(line, key, klen) == 0 &&
		    (line[klen] == ' ' || line[klen] == '='))
			return 1;
		i += n + 1;
	}
	return 0;
}

static int put(uint8_t *dst, uint32_t cap, uint32_t *pos, const void *src, uint32_t len)
{
	if (len > cap || *pos > cap - len)
		return -1;
	memcpy(dst + *pos, src, len);
	*pos += len;
	return 0;
}

int bootconfig_attach(char *cmdline, uint32_t cmdline_cap, uint8_t *dst, uint32_t cap)
{
	char *in = cmdline, *out = cmdline;
	const char *vbc;
	uint32_t pos = 0, vbc_size = 0, sum = 0, i, word[2];

	/* The vendor's own settings first, as a version 4 bootloader places them. */
	vbc = vendor_boot_bootconfig(&vbc_size);
	if (vbc && vbc_size) {
		uint32_t n = vbc_size;

		while (n && vbc[n - 1] == '\0')
			n--;
		if (put(dst, cap, &pos, vbc, n) != 0)
			return -1;
		if (n && vbc[n - 1] != '\n' && put(dst, cap, &pos, "\n", 1) != 0)
			return -1;
	}

	/* Every androidboot.key=value word leaves the command line for a key = "value" line. The
	 * first one of a key wins, as it does when init reads the command line. */
	while (*in) {
		const char *end;

		while (*in == ' ')
			in++;
		if (!*in)
			break;
		end = word_end(in);
		if (strncmp(in, ANDROIDBOOT, ANDROIDBOOT_LEN) == 0) {
			const char *eq = memchr(in, '=', end - in);
			uint32_t klen = eq ? (uint32_t)(eq - in) : (uint32_t)(end - in);
			const char *val = eq ? eq + 1 : end;
			uint32_t vlen = (uint32_t)(end - val);

			if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
				val++;
				vlen -= 2;
			}
			if (memchr(val, '"', vlen)) {
				pal_log_err("[bootconfig] %.*s: a quote in the value, left out\n", klen, in);
			} else if (!has_key((const char *)dst, pos, in, klen)) {
				if (put(dst, cap, &pos, in, klen) != 0 ||
				    put(dst, cap, &pos, " = \"", 4) != 0)
					return -1;
				if (klen == sizeof(BOOT_DEVICES_KEY) - 1 &&
				    memcmp(in, BOOT_DEVICES_KEY, klen) == 0) {
					/* the boot devices as a bootconfig array ("a", "b", ...), as the stock
					 * LK writes them: init reads this key back split at the spaces between
					 * the items, so one quoted "a,b,c" would be a single device name and
					 * no /dev/block/by-name link would ever be made */
					uint32_t k;

					for (k = 0; k < vlen; k++) {
						if (val[k] == ',') {
							if (put(dst, cap, &pos, "\", \"", 4) != 0)
								return -1;
						} else if (put(dst, cap, &pos, val + k, 1) != 0) {
							return -1;
						}
					}
				} else if (put(dst, cap, &pos, val, vlen) != 0) {
					return -1;
				}
				if (put(dst, cap, &pos, "\"\n", 2) != 0)
					return -1;
			}
		} else {
			uint32_t n = (uint32_t)(end - in);

			if (out != cmdline)
				*out++ = ' ';
			memmove(out, in, n);
			out += n;
		}
		in = (char *)end;
	}
	*out = '\0';

	/* "bootconfig" tells the kernel to look for the block. */
	if ((uint32_t)(out - cmdline) + sizeof(" bootconfig") > cmdline_cap)
		return -1;
	strcpy(out, out == cmdline ? "bootconfig" : " bootconfig");

	/* NULs up to a multiple of 4, then size, checksum and magic. */
	while (pos & 3) {
		if (put(dst, cap, &pos, "", 1) != 0)
			return -1;
	}
	for (i = 0; i < pos; i++)
		sum += dst[i];
	word[0] = pos;
	word[1] = sum;
	if (put(dst, cap, &pos, word, sizeof(word)) != 0 ||
	    put(dst, cap, &pos, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN) != 0)
		return -1;
	return (int)pos;
}
