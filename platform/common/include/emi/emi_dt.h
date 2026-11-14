/*
 * emi_dt.h - DRAM and EMI information for Android 12 era kernels (emi_dt.c).
 *
 * Android 12 preloaders pass the EMI boot tag (0x88610020) with a longer payload than older ones:
 * the DRAM type, channel and rank counts as bytes, the mode register and frequency tables, the
 * rank sizes and the EMI ISU buffer, ending with a version word. Their kernels read these back
 * from /dramc, /emicen, /emimpu and /emiisu, which the LK fills before the jump.
 */
#ifndef __EMI_DT_H__
#define __EMI_DT_H__

#include <stdint.h>

#define EMI_TAG_V2_VERSION   0x20190831
#define EMI_TAG_V2_SIZE      80

struct emi_tag_v2 {
	uint8_t  dram_type;          /* +0x00 */
	uint8_t  support_ch_cnt;     /* +0x01 */
	uint8_t  ch_cnt;             /* +0x02 */
	uint8_t  rk_cnt;             /* +0x03 */
	uint16_t mr_cnt;             /* +0x04 */
	uint16_t freq_cnt;           /* +0x06 */
	uint64_t rk_size[2];         /* +0x08, bytes */
	uint32_t freq_step[3];       /* +0x18, MT/s */
	uint16_t mr[2];              /* +0x24 */
	uint32_t isu_buf_size;       /* +0x28 */
	uint32_t pad0;               /* +0x2c */
	uint64_t isu_buf_addr;       /* +0x30 */
	uint64_t isu_ver_addr;       /* +0x38 */
	uint64_t isu_con_addr;       /* +0x40 */
	uint32_t version;            /* +0x48, EMI_TAG_V2_VERSION */
	uint32_t pad1;               /* +0x4c */
} __attribute__((packed));

/* Keep the payload of the preloader's EMI boot tag (len bytes). */
void emi_tag_v2_capture(const void *payload, uint32_t len);

/* Fill /dramc, /emicen, /emimpu and /emiisu of the kernel device tree. 0 or a libfdt error. */
int set_fdt_emi_info(void *fdt);

#endif /* __EMI_DT_H__ */
