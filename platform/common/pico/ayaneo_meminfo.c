/*
 * Dump the physical DRAM map (ranks + mblock usable blocks + reserved regions) so we can pick a
 * safe high-DRAM region to claim for the emulator rewind ring buffer. Read-only. The reserved[]
 * list (with names) is the authoritative "do not touch" map from the preloader/mblock.
 */
#include <platform/mt_typedefs.h>
#include <platform/boot_mode.h>
#include <printf.h>

extern BOOT_ARGUMENT *g_boot_arg;
extern u64 physical_memory_size(void);

/* print a u64 as hi/lo hex (avoids relying on %ll in the LK printf) */
#define HL(v) (unsigned int)((unsigned long long)(v) >> 32), (unsigned int)((unsigned long long)(v))

void ayaneo_meminfo_dump(void (*emit)(const char *line))
{
	char b[96];
	unsigned int i;
	unsigned long long total;
	dram_info_t *di;
	mblock_info_t *mi;

	if (!g_boot_arg) { emit("meminfo: no g_boot_arg"); return; }
	total = physical_memory_size();
	di = &g_boot_arg->orig_dram_info;
	mi = &g_boot_arg->mblock_info;

	snprintf(b, sizeof b, "DRAM base=0x40000000 total=0x%x%08x (%uMB)", HL(total),
		 (unsigned int)(total >> 20));
	emit(b);
	for (i = 0; i < di->rank_num && i < 4u; i++) {
		snprintf(b, sizeof b, "rank%u 0x%x%08x +0x%x%08x", i,
			 HL(di->rank_info[i].start), HL(di->rank_info[i].size));
		emit(b);
	}
	snprintf(b, sizeof b, "mblock_num=%u reserved_num=%u magic=0x%x", mi->mblock_num,
		 mi->reserved_num, mi->mblock_magic);
	emit(b);
	for (i = 0; i < mi->mblock_num && i < 24u; i++) {
		snprintf(b, sizeof b, "mblk%u 0x%x%08x +0x%x%08x", i,
			 HL(mi->mblock[i].start), HL(mi->mblock[i].size));
		emit(b);
	}
	for (i = 0; i < mi->reserved_num && i < 80u; i++) {
		snprintf(b, sizeof b, "rsv 0x%x%08x +0x%x%08x %s",
			 HL(mi->reserved[i].start), HL(mi->reserved[i].size), mi->reserved[i].name);
		emit(b);
	}
}
