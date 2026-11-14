/*
 * emi_dt.c - the DRAM and EMI nodes of an Android 12 era kernel device tree, filled from the
 * preloader's EMI boot tag (emi_dt.h).
 *
 * Every value is written as the kernel drivers read it: 32-bit cells and 64-bit pairs of cells,
 * big endian. Each node is looked for at the root first, then under /soc, as the boards place
 * them either way. Rank sizes in /dramc are in units of 128 MB (1 Gbit); /emicen keeps them in
 * bytes. A node that is missing is skipped: its driver then reports the missing values itself.
 */
#include <string.h>
#include <debug.h>
#include <libfdt.h>
#include <emi/emi_dt.h>

static struct emi_tag_v2 s_emi;
static int s_emi_ok;

void emi_tag_v2_capture(const void *payload, uint32_t len)
{
	if (len < EMI_TAG_V2_SIZE) {
		dprintf(CRITICAL, "[EMI] boot tag of %u bytes, not the Android 12 layout\n", len);
		return;
	}
	memcpy(&s_emi, payload, sizeof(s_emi));
	s_emi_ok = (s_emi.version == EMI_TAG_V2_VERSION);
	if (!s_emi_ok)
		dprintf(CRITICAL, "[EMI] boot tag version 0x%x, expected 0x%x\n",
			s_emi.version, EMI_TAG_V2_VERSION);
}

static int node(void *fdt, const char *a, const char *b)
{
	int off = fdt_path_offset(fdt, a);

	return off >= 0 ? off : fdt_path_offset(fdt, b);
}

static void cell(uint32_t *dst, uint32_t v)
{
	*dst = cpu_to_fdt32(v);
}

static void pair(uint32_t *dst, uint64_t v)
{
	dst[0] = cpu_to_fdt32((uint32_t)(v >> 32));
	dst[1] = cpu_to_fdt32((uint32_t)v);
}

static int prop_cell(void *fdt, int off, const char *name, uint32_t v)
{
	uint32_t c;

	cell(&c, v);
	return fdt_setprop(fdt, off, name, &c, sizeof(c));
}

static int prop_pair(void *fdt, int off, const char *name, uint64_t v)
{
	uint32_t c[2];

	pair(c, v);
	return fdt_setprop(fdt, off, name, c, sizeof(c));
}

static int set_dramc(void *fdt)
{
	uint32_t rk[2], mr[2], freq[3];
	int off, i, ret;

	off = node(fdt, "/dramc", "/soc/dramc");
	if (off < 0)
		return off;
	/* both rank entries as the preloader filled them, whatever the rank count */
	for (i = 0; i < 2; i++)
		cell(&rk[i], s_emi.rk_cnt ? (uint32_t)(s_emi.rk_size[i] >> 27) : 0);
	for (i = 0; i < 2; i++)
		cell(&mr[i], s_emi.mr_cnt ? s_emi.mr[i] : 0);
	for (i = 0; i < 3; i++)
		cell(&freq[i], i < s_emi.freq_cnt ? s_emi.freq_step[i] : 0);

	if ((ret = prop_cell(fdt, off, "dram_type", s_emi.dram_type)) ||
	    (ret = prop_cell(fdt, off, "support_ch_cnt", s_emi.support_ch_cnt)) ||
	    (ret = prop_cell(fdt, off, "ch_cnt", s_emi.ch_cnt)) ||
	    (ret = prop_cell(fdt, off, "rk_cnt", s_emi.rk_cnt)) ||
	    (ret = prop_cell(fdt, off, "mr_cnt", s_emi.mr_cnt)) ||
	    (ret = prop_cell(fdt, off, "freq_cnt", s_emi.freq_cnt)) ||
	    (ret = fdt_setprop(fdt, off, "rk_size", rk, sizeof(rk))) ||
	    (ret = fdt_setprop(fdt, off, "mr", mr, sizeof(mr))))
		return ret;
	return fdt_setprop(fdt, off, "freq_step", freq, sizeof(freq));
}

static int set_emicen_emimpu(void *fdt)
{
	uint32_t rk[4];
	uint64_t total = 0;
	int off, i, ret;

	off = node(fdt, "/soc/emicen", "/emicen");
	if (off < 0)
		return off;
	for (i = 0; i < 2; i++) {
		uint64_t size = i < s_emi.rk_cnt ? s_emi.rk_size[i] : 0;

		pair(&rk[i * 2], size);
		total += size;
	}
	if ((ret = prop_cell(fdt, off, "ch_cnt", s_emi.ch_cnt)) ||
	    (ret = prop_cell(fdt, off, "rk_cnt", s_emi.rk_cnt)) ||
	    (ret = fdt_setprop(fdt, off, "rk_size", rk, sizeof(rk))))
		return ret;

	/* the DRAM the EMI MPU guards: from the start of DRAM through every rank */
	off = node(fdt, "/soc/emimpu", "/emimpu");
	if (off < 0)
		return off;
	if ((ret = prop_pair(fdt, off, "dram_start", 0x40000000ULL)))
		return ret;
	return prop_pair(fdt, off, "dram_end", 0x40000000ULL + total - 1);
}

static int set_emiisu(void *fdt)
{
	int off, ret;

	off = node(fdt, "/soc/emiisu", "/emiisu");
	if (off < 0)
		return off;
	if ((ret = prop_cell(fdt, off, "buf_size", s_emi.isu_buf_size)) ||
	    (ret = prop_pair(fdt, off, "buf_addr", s_emi.isu_buf_addr)) ||
	    (ret = prop_pair(fdt, off, "ver_addr", s_emi.isu_ver_addr)))
		return ret;
	return prop_pair(fdt, off, "con_addr", s_emi.isu_con_addr);
}

int set_fdt_emi_info(void *fdt)
{
	int ret, r;

	if (!fdt)
		return -1;
	if (!s_emi_ok) {
		dprintf(CRITICAL, "[EMI] no Android 12 EMI boot tag: /dramc and /emi* left empty\n");
		return -1;
	}
	dprintf(INFO, "[EMI] type %u, %u/%u channels, %u ranks (0x%llx, 0x%llx), %u steps\n",
		s_emi.dram_type, s_emi.ch_cnt, s_emi.support_ch_cnt, s_emi.rk_cnt,
		s_emi.rk_size[0], s_emi.rk_size[1], s_emi.freq_cnt);
	ret = set_dramc(fdt);
	if (ret)
		dprintf(CRITICAL, "[EMI] /dramc: %d\n", ret);
	r = set_emicen_emimpu(fdt);
	if (r)
		dprintf(CRITICAL, "[EMI] /emicen, /emimpu: %d\n", r);
	ret = ret ? ret : r;
	r = set_emiisu(fdt);
	if (r)
		dprintf(CRITICAL, "[EMI] /emiisu: %d\n", r);
	return ret ? ret : r;
}
