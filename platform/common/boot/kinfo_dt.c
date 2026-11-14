/*
 * kinfo_dt.c - the /debug-kinfo node of Android 12 era kernels: the google,debug-kinfo driver
 * keeps the kernel's symbol and layout information in a reserved block that the crash tools find
 * after a reset. The preloader reserves that block (mblock "aee_debug_kinfo"); the LK has turned
 * it into /reserved-memory/mblock-N-aee_debug_kinfo by now, and here points /debug-kinfo at it.
 */
#include <debug.h>
#include <libfdt.h>

#define KINFO_REGION_COMPATIBLE "mediatek,aee_debug_kinfo"

/* The largest phandle in the tree, so a new one does not collide. */
static uint32_t max_phandle(const void *fdt)
{
	uint32_t max = 0;
	int off;

	for (off = fdt_next_node(fdt, -1, NULL); off >= 0; off = fdt_next_node(fdt, off, NULL)) {
		uint32_t p = fdt_get_phandle(fdt, off);

		if (p != (uint32_t)-1 && p > max)
			max = p;
	}
	return max;
}

int fdt_debug_kinfo_append(void *fdt)
{
	uint32_t phandle, cell;
	int region, node, ret;

	region = fdt_node_offset_by_compatible(fdt, -1, KINFO_REGION_COMPATIBLE);
	if (region < 0) {
		dprintf(CRITICAL, "[kinfo] no reserved debug-kinfo region\n");
		return region;
	}
	phandle = fdt_get_phandle(fdt, region);
	if (phandle == 0 || phandle == (uint32_t)-1) {
		phandle = max_phandle(fdt) + 1;
#ifdef KONKR_BLOCK
		cell = phandle;	/* native order, the same as the reference below (see there) */
#else
		cell = cpu_to_fdt32(phandle);
#endif
		ret = fdt_setprop(fdt, region, "phandle", &cell, sizeof(cell));
		if (ret)
			return ret;
	}

	node = fdt_path_offset(fdt, "/debug-kinfo");
	if (node < 0) {
		node = fdt_add_subnode(fdt, 0, "debug-kinfo");
		if (node < 0)
			return node;
	}
	ret = fdt_setprop_string(fdt, node, "compatible", "google,debug-kinfo");
	if (ret)
		return ret;
#ifdef KONKR_BLOCK
	/* As the Block's stock LK writes them: the region's phandle and this reference to it both
	 * in native (little endian) order. They still match each other, so the kernel resolves
	 * the region; only the value differs from a big endian cell. */
	cell = phandle;
#else
	cell = cpu_to_fdt32(phandle);
#endif
	return fdt_setprop(fdt, node, "memory-region", &cell, sizeof(cell));
}
