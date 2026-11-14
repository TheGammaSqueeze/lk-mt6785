/*
 * bootimg_v3.h - Android boot images with header version 3 and 4 (Android 11 and later): a boot
 * partition with the kernel and the generic ramdisk, a vendor_boot partition with the vendor
 * ramdisk, the device tree, the vendor command line and (version 4) a bootconfig section.
 *
 * The rest of the LK keeps working on the legacy header (struct bootimg_hdr): a version 3 or 4
 * boot is described to it as a legacy image whose ramdisk is the vendor ramdisk followed by the
 * generic one, whose command line is the vendor's followed by the boot image's, and whose device
 * tree comes from vendor_boot. The androidboot.* settings go to the kernel as a bootconfig block
 * after the ramdisks (see bootconfig_attach).
 */
#ifndef __BOOTIMG_V3_H__
#define __BOOTIMG_V3_H__

#include <stdint.h>
#include <bootimg.h>

#define BOOT_V3_PAGE_SIZE        4096
#define BOOT_V3_CMDLINE_SIZE     1536
#define VENDOR_BOOT_MAGIC        "VNDRBOOT"
#define VENDOR_BOOT_MAGIC_SIZE   8
#define VENDOR_BOOT_CMDLINE_SIZE 2048
#define VENDOR_BOOT_NAME_SIZE    16

/* Room kept after the ramdisks for the bootconfig block. */
#define BOOTCONFIG_ROOM          (64 * 1024)

struct boot_v3_hdr {
	uint8_t  magic[BOOTIMG_MAGIC_SZ];     /* "ANDROID!" */
	uint32_t kernel_size;
	uint32_t ramdisk_size;                /* the generic ramdisk */
	uint32_t os_version;
	uint32_t header_size;
	uint32_t reserved[4];
	uint32_t header_version;              /* at the same offset as in versions 0 to 2 */
	uint8_t  cmdline[BOOT_V3_CMDLINE_SIZE];
	uint32_t signature_size;              /* version 4 */
} __attribute__((packed));

struct vendor_boot_hdr {
	uint8_t  magic[VENDOR_BOOT_MAGIC_SIZE];
	uint32_t header_version;
	uint32_t page_size;
	uint32_t kernel_addr;
	uint32_t ramdisk_addr;
	uint32_t vendor_ramdisk_size;
	uint8_t  cmdline[VENDOR_BOOT_CMDLINE_SIZE];
	uint32_t tags_addr;
	uint8_t  name[VENDOR_BOOT_NAME_SIZE];
	uint32_t header_size;
	uint32_t dtb_size;
	uint64_t dtb_addr;
	/* version 4 */
	uint32_t ramdisk_table_size;
	uint32_t ramdisk_table_entry_num;
	uint32_t ramdisk_table_entry_size;
	uint32_t bootconfig_size;
} __attribute__((packed));

/* The vendor_boot image the boot uses, as loaded in memory (verified by AVB). */
void vendor_boot_set_image(const void *image, uint32_t size);
const void *vendor_boot_image(void);

/* 1 when the boot image header at hdr is version 3 or later. */
int bootimg_is_v3(const void *hdr);

/* Describe a version 3/4 boot image (boot, its header at the start) with the vendor_boot image set
 * by vendor_boot_set_image as a legacy header. 0, or -1 when either header is unusable. */
int bootimg_v3_to_legacy(const void *boot, struct bootimg_hdr *legacy);

/* The same from the headers only, read from the partitions (the image is not loaded yet). */
int bootimg_v3_hdr_from_partitions(const char *boot_part, const char *vendor_part,
				   struct bootimg_hdr *legacy);

/* The vendor ramdisk (every fragment of version 4), the device tree section and the vendor
 * bootconfig section of the vendor_boot image set by vendor_boot_set_image. */
const void *vendor_boot_ramdisk(uint32_t *size);
const void *vendor_boot_dtb(uint32_t *size);
const void *vendor_boot_bootconfig(uint32_t *size);

/* The full command line for a version 3/4 image: the vendor's, then the boot image's. */
const char *bootimg_v3_cmdline(void);

/*
 * Move every androidboot.* setting of the kernel command line in cmdline (a NUL-terminated string
 * edited in place) into a bootconfig block written at dst, followed by the vendor bootconfig
 * section, the size and checksum and the "#BOOTCONFIG\n" trailer, and add "bootconfig" to the
 * command line. cap is the room at dst, cmdline_cap the size of the command line buffer. Returns
 * the bytes written at dst, or -1 when the block or the command line does not fit.
 */
int bootconfig_attach(char *cmdline, uint32_t cmdline_cap, uint8_t *dst, uint32_t cap);

#endif /* __BOOTIMG_V3_H__ */
