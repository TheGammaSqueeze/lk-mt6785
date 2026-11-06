/*
 * Copyright (c) 2008 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <debug.h>
#include <sys/types.h>
#include <compiler.h>
#include <arch.h>
#include <arch/arm.h>
#include <arch/arm/mmu.h>
#include <platform/mt_reg_base.h>
#include <stdlib.h>
#include <string.h>
#include <arch/ops.h>
#include <lib/heap.h>
#include <err.h>

#if ARM_WITH_MMU
#ifdef MTK_3LEVEL_PAGETABLE

ld_tt_l2_info_t ld_tt_l2_info;
uint64_t ld_tt_l1[4] __ALIGNED(32);

/* convert user level mmu flags to flags that go in L1 descriptors */
static uint64_t mmu_flags_to_l1_arch_flags(uint flags)
{
	uint64_t arch_flags = 0;
	switch (flags & MMU_MEMORY_TYPE_MASK) {
		case MMU_MEMORY_TYPE_STRONGLY_ORDERED:
			arch_flags |= MMU_MEMORY_TYPE_STRONGLY_ORDERED;
			/* strongly order & device memory should be marked as XN */
			arch_flags |= MMU_L1_MEMORY_XN;
			break;
		case MMU_MEMORY_TYPE_NORMAL:
			arch_flags |= MMU_MEMORY_TYPE_NORMAL;
			break;
		case MMU_MEMORY_TYPE_NORMAL_WRITE_THROUGH:
			arch_flags |= MMU_MEMORY_TYPE_NORMAL_WRITE_THROUGH;
			break;
		case MMU_MEMORY_TYPE_DEVICE:
			arch_flags |= MMU_MEMORY_TYPE_DEVICE;
			/* strongly order & device memory should be marked as XN */
			arch_flags |= MMU_L1_MEMORY_XN;
			break;
		case MMU_MEMORY_TYPE_NORMAL_WRITE_BACK:
			arch_flags |= MMU_MEMORY_TYPE_NORMAL_WRITE_BACK;
			break;
	}

	switch (flags & MMU_MEMORY_AP_MASK) {
		case MMU_MEMORY_AP_P_RW_U_NA:
			arch_flags |= MMU_MEMORY_AP_P_RW_U_NA;
			break;
		case MMU_MEMORY_AP_P_RW_U_RW:
			arch_flags |= MMU_MEMORY_AP_P_RW_U_RW;
			break;
		case MMU_MEMORY_AP_P_R_U_NA:
			arch_flags |= MMU_MEMORY_AP_P_R_U_NA;
			break;
		case MMU_MEMORY_AP_P_R_U_R:
			arch_flags |= MMU_MEMORY_AP_P_R_U_R;
			break;
	}
	if (flags & MMU_MEMORY_ATTRIBUTE_XN) {

		arch_flags |= MMU_L1_MEMORY_XN;
	}
	arch_flags |= MMU_MEMORY_L1_AF;
	return arch_flags;
}

/* todo: table walk soultion */
uint64_t arm_mmu_va2pa(unsigned int vaddr)
{
	uint64_t paddr;
	/* trick, only work when paddr < 4G*/
	paddr = (uint64_t)vaddr;
	return paddr;
}

void mmu_update_tt_entry(uint64_t* tt_entry, uint64_t value)
{
	/* Get the index into the translation table */
	*tt_entry = value;
	//arch_clean_invalidate_cache_range(tt_entry, CACHE_LINE);
	arch_clean_cache_range((addr_t)tt_entry, CACHE_LINE);
}
/* A block split into a table of the next level (to map part of it differently) starts as a copy of
 * the block: every entry maps the same addresses with the same attributes, so the split itself
 * unmaps nothing and only the entries the caller then rewrites change. (A fresh table is all
 * no-access, which unmapped the rest of the block.) step = the size one entry of the new table
 * maps; the last level's entries are pages (type 3), the others blocks (type 1). */
static void tt_fill_from_block(uint64_t *table, uint64_t old, uint64_t step, int last_level)
{
	uint i;
	uint64_t attrs = old & 0xFFF0000000000FFCull;
	uint64_t base = old & 0x000000FFFFFFF000ull & ~(step * 512u - 1u);
	for (i = 0; i < 512u; i++)
		table[i] = (base + (uint64_t)i * step) | attrs | (last_level ? 3u : 1u);
	arch_clean_cache_range((addr_t)table, PAGE_SIZE);
}

static status_t get_l2_table(uint32_t l1_index, uint64_t *ppa)
{
	uint64_t pa;
	uint32_t *l2_va = NULL;
	DEBUG_ASSERT(ppa);

	/* allocate from static pool before heap init */
	if (ld_tt_l2_info.heap_init_done == 0) {
		if (ld_tt_l2_info.index < LD_TT_L2_STATIC_SIZE)
			l2_va = (void *)&ld_tt_l2_info.ld_tt_l2[ld_tt_l2_info.index++][0];
		else
			dprintf(CRITICAL, "mmu static pool too small!\n");
	} else {
		l2_va = heap_alloc(PAGE_SIZE, PAGE_SIZE); //malloc(PAGE_SIZE);
	}
	if (!l2_va)
		return ERR_NO_MEMORY;

	/* wipe it clean to set no access */
	memset(l2_va, 0, PAGE_SIZE);

	/* get physical address */
	pa = arm_mmu_va2pa((uint32_t)l2_va);
	*ppa = pa;
	return NO_ERROR;
}


/* Install a caller-provided, zeroed, 4 KB aligned page as the second level table of one 1 GB
 * range whose first level entry is still invalid or a block. arch_mmu_map then fills it with 2 MB
 * sections instead of allocating the table from the heap. Returns 0 when installed, -1 when the
 * range already has a table (left as is). */
int arch_mmu_install_l2(uint32_t l1_index, void *page)
{
	uint64_t e1 = ld_tt_l1[l1_index & 3u];
	if ((e1 & MMU_MEMORY_L1_DESCRIPTOR_MASK) == MMU_MEMORY_L1_DESCRIPTOR_TABLE) return -1;
	memset(page, 0, PAGE_SIZE);
	arch_clean_cache_range((addr_t)page, PAGE_SIZE);
	mmu_update_tt_entry(&ld_tt_l1[l1_index & 3u], ((uint64_t)(addr_t)page) | MMU_MEMORY_L1_DESCRIPTOR_TABLE);
	return 0;
}

/* Walk the LPAE tables for one VA: returns 1 (1GB block), 2 (2MB section), 3 (table->page level,
 * reported as mapped), 0 unmapped. l1/l2 receive the raw descriptors for a diagnostic print. */
int arch_mmu_probe(vaddr_t vaddr, uint64_t *l1, uint64_t *l2)
{
	uint64_t e1 = ld_tt_l1[vaddr / BLOCK_SIZE], e2 = 0;
	int kind = 0;
	switch (e1 & MMU_MEMORY_L1_DESCRIPTOR_MASK) {
	case MMU_MEMORY_L1_DESCRIPTOR_BLOCK: kind = 1; break;
	case MMU_MEMORY_L1_DESCRIPTOR_TABLE: {
		uint64_t *t = (uint64_t *)(addr_t)(e1 & 0xFFFFF000ULL);   /* table PA, identity mapped */
		e2 = t[(vaddr / SECTION_SIZE) % 512u];
		kind = (e2 & 3u) == 1u ? 2 : (e2 & 3u) == 3u ? 3 : 0;
		break;
	}
	default: kind = 0;
	}
	if (l1) *l1 = e1;
	if (l2) *l2 = e2;
	return kind;
}

/* the last eight map requests (vaddr, count, ms on the 13 MHz clock): who maps at run time */
volatile unsigned int g_dbg_mmu_log[8][3], g_dbg_mmu_log_n;
int arch_mmu_map(uint64_t paddr, vaddr_t vaddr, uint flags, uint count)
{
	int ret = NO_ERROR;
	{
		extern unsigned gpt4_get_current_tick(void);
		unsigned int k = g_dbg_mmu_log_n % 8u;
		g_dbg_mmu_log[k][0] = vaddr; g_dbg_mmu_log[k][1] = count; g_dbg_mmu_log[k][2] = gpt4_get_current_tick() / 13000u;
		g_dbg_mmu_log_n++;
	}
	/* paddr and vaddr must be aligned */
	DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
	DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
	DEBUG_ASSERT(IS_PAGE_ALIGNED(count));
	if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(paddr) || !IS_PAGE_ALIGNED(count))
		return ERR_INVALID_ARGS;

	if (count == 0)
		return NO_ERROR;

	while (count > 0) {
		if (IS_BLOCK_ALIGNED(vaddr) && IS_BLOCK_ALIGNED(paddr) && count >= BLOCK_SIZE) {
			/* can use a block,  overwrite it! */
			uint l1_index = vaddr / BLOCK_SIZE;
			/* compute the arch flags for L1 sections */
			uint64_t arch_flags = mmu_flags_to_l1_arch_flags(flags) |
			                      MMU_MEMORY_L1_DESCRIPTOR_BLOCK;
			/* map it , 1GB  */
			mmu_update_tt_entry(&ld_tt_l1[l1_index], paddr | arch_flags);
			count -= BLOCK_SIZE;
			vaddr += BLOCK_SIZE;
			paddr += BLOCK_SIZE;
		} else if (IS_SECTION_ALIGNED(vaddr) && IS_SECTION_ALIGNED(paddr) && count >= SECTION_SIZE) {
			uint l1_index = vaddr / BLOCK_SIZE;
			uint64_t tt_entry = ld_tt_l1[l1_index];
			switch (tt_entry & MMU_MEMORY_L1_DESCRIPTOR_MASK) {
				case MMU_MEMORY_L1_DESCRIPTOR_BLOCK:
				case MMU_MEMORY_L1_DESCRIPTOR_INVALID: {
					uint64_t l2_pa = 0;
					if ((ret = get_l2_table(l1_index, &l2_pa)) != NO_ERROR) {
						dprintf(CRITICAL, "failed to allocate l2 pagetable\n");
						goto _done;
					}
					if ((tt_entry & MMU_MEMORY_L1_DESCRIPTOR_MASK) == MMU_MEMORY_L1_DESCRIPTOR_BLOCK)
						tt_fill_from_block((uint64_t *)(uint32_t)l2_pa, tt_entry, SECTION_SIZE, 0);
					tt_entry = l2_pa | MMU_MEMORY_L1_DESCRIPTOR_TABLE;
					mmu_update_tt_entry(&ld_tt_l1[l1_index], tt_entry);
					/* fallthrough */
				}
				case MMU_MEMORY_L1_DESCRIPTOR_TABLE: {
					uint64_t* l2_table = (uint64_t *)(uint32_t)(tt_entry & 0xfffff000);
					DEBUG_ASSERT(l2_table);
					/* compute the arch flags for L2 2MB */
					uint arch_flags = mmu_flags_to_l1_arch_flags(flags)| MMU_MEMORY_L1_DESCRIPTOR_BLOCK;;
					uint l2_index = (vaddr / SECTION_SIZE) & 0x1ff;
					mmu_update_tt_entry(&l2_table[l2_index], paddr | arch_flags);
					count -= SECTION_SIZE;
					vaddr += SECTION_SIZE;
					paddr += SECTION_SIZE;
					break;
				}
				default:
					PANIC_UNIMPLEMENTED;
			}
		} else if (IS_PAGE_ALIGNED(vaddr) && IS_PAGE_ALIGNED(paddr) && count >= PAGE_SIZE) {
			uint l1_index = vaddr / BLOCK_SIZE;
			uint64_t tt_entry = ld_tt_l1[l1_index];
			switch (tt_entry & MMU_MEMORY_L1_DESCRIPTOR_MASK) {
				case MMU_MEMORY_L1_DESCRIPTOR_BLOCK:
				case MMU_MEMORY_L1_DESCRIPTOR_INVALID: {
					uint64_t l2_pa = 0;
					if ((ret = get_l2_table(l1_index, &l2_pa)) != NO_ERROR) {
						dprintf(CRITICAL, "failed to allocate l2 pagetable\n");
						goto _done;
					}
					if ((tt_entry & MMU_MEMORY_L1_DESCRIPTOR_MASK) == MMU_MEMORY_L1_DESCRIPTOR_BLOCK)
						tt_fill_from_block((uint64_t *)(uint32_t)l2_pa, tt_entry, SECTION_SIZE, 0);
					tt_entry = l2_pa | MMU_MEMORY_L1_DESCRIPTOR_TABLE;
					mmu_update_tt_entry(&ld_tt_l1[l1_index], tt_entry);
					/* fallthrough */
				}
				case MMU_MEMORY_L1_DESCRIPTOR_TABLE: {

					uint64_t* l2_table = (uint64_t *)(uint32_t)(tt_entry & 0xfffff000);
					DEBUG_ASSERT(l2_table);

					uint l2_index = (vaddr / SECTION_SIZE) & 0x1ff;
					uint64_t tt_entry = l2_table[l2_index];
					switch (tt_entry & MMU_MEMORY_L1_DESCRIPTOR_MASK) {
						case MMU_MEMORY_L1_DESCRIPTOR_BLOCK:
						case MMU_MEMORY_L1_DESCRIPTOR_INVALID: {
							uint64_t l3_pa = 0;
							if ((ret = get_l2_table(l1_index, &l3_pa)) != NO_ERROR) {
								dprintf(CRITICAL, "failed to allocate l3 pagetable\n");
								goto _done;
							}
							if ((tt_entry & MMU_MEMORY_L1_DESCRIPTOR_MASK) == MMU_MEMORY_L1_DESCRIPTOR_BLOCK)
								tt_fill_from_block((uint64_t *)(uint32_t)l3_pa, tt_entry, PAGE_SIZE, 1);
							tt_entry = l3_pa | MMU_MEMORY_L1_DESCRIPTOR_TABLE;
							mmu_update_tt_entry(&l2_table[l2_index], tt_entry);
							/* fallthrough */
						}
						case MMU_MEMORY_L1_DESCRIPTOR_TABLE: {
							uint64_t* l3_table = (uint64_t *)(uint32_t)(tt_entry & 0xfffff000);
							DEBUG_ASSERT(l3_table);
							/* compute the arch flags for L3 4KB */
							uint arch_flags = mmu_flags_to_l1_arch_flags(flags)| MMU_MEMORY_L1_DESCRIPTOR_TABLE;;
							uint l3_index = (vaddr / PAGE_SIZE) & 0x1ff;
							mmu_update_tt_entry(&l3_table[l3_index], paddr | arch_flags);
							count -= PAGE_SIZE;
							vaddr += PAGE_SIZE;
							paddr += PAGE_SIZE;
							break;
						}
						default:
							PANIC_UNIMPLEMENTED;
					}
					break;
				}
				default:
					PANIC_UNIMPLEMENTED;
			}
		} else
			DEBUG_ASSERT(0);

	}
_done:
	arm_invalidate_tlb();
	DSB;
	ISB;
	return ret;
}

void arm_mmu_lpae_init(void)
{
	unsigned int ttbcr = (SREG_TTBCR_EAE | SREG_TTBCR_SH1 | SREG_TTBCR_SH0);
	unsigned int mair0 = MMU_MAIR0;
	unsigned int mair1 = MMU_MAIR1;

	/* set some mmu specific control bits:
	 * access flag disabled, TEX remap disabled, mmu disabled
	 */
	arm_write_cr1(arm_read_cr1() & ~(SREG_SCTLR_AFE|SREG_SCTLR_TRE|SREG_SCTLR_M));

	__asm__ volatile("mcr p15, 0, %0, c2,  c0, 2" :: "r" (ttbcr));
	__asm__ volatile("mcr p15, 0, %0, c10, c2, 0" :: "r" (mair0));
	__asm__ volatile("mcr p15, 0, %0, c10, c2, 1" :: "r" (mair1));

	/* set up the translation table base */
	arm_write_ttbr((uint32_t)ld_tt_l1);

	/* set up the domain access register */
	arm_write_dacr(0x00000001);
}

void arch_enable_mmu(void)
{
	arm_write_cr1(arm_read_cr1() | SREG_SCTLR_M);
}

void arch_disable_mmu(void)
{
	arm_write_cr1(arm_read_cr1() & ~SREG_SCTLR_M);
}

#else	//!MTK_3LEVEL_PAGETABLE
#define MB (1024*1024)
#define GB (1024*1024*1024)

/* the location of the table may be brought in from outside */
#if WITH_EXTERNAL_TRANSLATION_TABLE
#if !defined(MMU_TRANSLATION_TABLE_ADDR)
#error must set MMU_TRANSLATION_TABLE_ADDR in the make configuration
#endif
static uint32_t *tt = (void *)MMU_TRANSLATION_TABLE_ADDR;
#else
/* the main translation table */
static uint32_t tt[4096] __ALIGNED(16384);
#endif
#ifndef MTK_LM_2LEVEL_PAGETABLE_MODE
static uint64_t *lpae_tt = (uint64_t *)tt;
#else
/* L1 must align it's table size */
static uint64_t ld_tt_l1[4] __ALIGNED(32);
static uint64_t *ld_tt_l2 = (uint64_t *)tt;
#endif
void arm_mmu_map_section(addr_t paddr, addr_t vaddr, uint flags)
{
	int index;

	/* Get the index into the translation table */
	index = vaddr / MB;

	/* Set the entry value:
	 * (2<<0): Section entry
	 * (0<<5): Domain = 0
	 *  flags: TEX, CB and AP bit settings provided by the caller.
	 */
	tt[index] = (paddr & ~(MB-1)) | (0<<5) | (2<<0) | flags;

	arm_invalidate_tlb();
}

void arm_mmu_map_block(unsigned long long paddr, addr_t vaddr, unsigned long long flags)
{
	/* Get the index into the translation table */
#ifndef MTK_LM_2LEVEL_PAGETABLE_MODE
	int index = vaddr / GB;

	lpae_tt[index] = (paddr & (0x000000FFC0000000ULL)) | (0x1<<10) | (0x3<<8) | (0x1<<0) | flags;
#else
	int index = vaddr / (2*MB);
	ld_tt_l2[index] = (paddr & (0x000000FFFFE00000ULL)) | (0x1<<10) | (0x3<<8) | (0x1<<0) | flags;
#endif
	arm_invalidate_tlb();
}

unsigned long long arm_mmu_va2pa(unsigned int vaddr)
{
	unsigned long long paddr;
	int index;

	unsigned int ttbcr;
	__asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r" (ttbcr));

	if (!(ttbcr & (0x1 << 31))) {
		index = vaddr / MB;
		paddr = (tt[index] & ~(MB-1)) | (vaddr & (MB-1));
	} else {
#ifndef MTK_LM_2LEVEL_PAGETABLE_MODE
		index = vaddr / GB;
		paddr = (lpae_tt[index] & (0x000000FFC0000000ULL)) | (vaddr & (GB-1));
#else
		index = vaddr / (2*MB);
		paddr = (ld_tt_l2[index] & (0x000000FFFFE00000ULL)) | (vaddr & ((2*MB)-1));
#endif
	}

	return paddr;
}

void arm_mmu_init(void)
{
	unsigned int i;

	//extern u64 physical_memory_size(void)__attribute__((weak));
	extern uint64_t physical_memory_size(void)__attribute__((weak));

	/* set some mmu specific control bits:
	 * access flag disabled, TEX remap disabled, mmu disabled
	 */
	arm_write_cr1(arm_read_cr1() & ~((1<<29)|(1<<28)|(1<<0)));

	if (physical_memory_size) {
		unsigned int dram_size = 0;
		unsigned int mapping = 0;
		uint64_t total_size = 0;

		dram_size = physical_memory_size();
		total_size = (uint64_t)DRAM_PHY_ADDR + (uint64_t)dram_size;
		mapping = total_size/(MB);
		if (mapping >= 4096)
			mapping = 4096;

		/* set up an identity-mapped translation table with
		 * strongly ordered memory type and read/write access.
		 */
		for (i=0; i < mapping; i++) {
			arm_mmu_map_section(i * MB,
			                    i * MB,
			                    MMU_MEMORY_TYPE_STRONGLY_ORDERED |
			                    MMU_MEMORY_AP_READ_WRITE);
		}
	}

	else {
		/* set up an identity-mapped translation table with
		   * strongly ordered memory type and read/write access.
		   */
		for (i=0; i < 4096; i++) {
			arm_mmu_map_section(i * MB,
			                    i * MB,
			                    MMU_MEMORY_TYPE_STRONGLY_ORDERED |
			                    MMU_MEMORY_AP_READ_WRITE);
		}
	}


	/* set up the translation table base */
	arm_write_ttbr((uint32_t)tt);

	/* set up the domain access register */
	arm_write_dacr(0x00000001);
}

void arm_mmu_lpae_init(void)
{
	unsigned int i;
	unsigned int ttbcr = 0xB0003000;
	unsigned int mair0 = 0xeeaa4400;
	unsigned int mair1 = 0xff000004;

	/* set some mmu specific control bits:
	 * access flag disabled, TEX remap disabled, mmu disabled
	 */
	arm_write_cr1(arm_read_cr1() & ~((1<<29)|(1<<28)|(1<<0)));

	__asm__ volatile("mcr p15, 0, %0, c2,  c0, 2" :: "r" (ttbcr));
	__asm__ volatile("mcr p15, 0, %0, c10, c2, 0" :: "r" (mair0));
	__asm__ volatile("mcr p15, 0, %0, c10, c2, 1" :: "r" (mair1));

#ifndef MTK_LM_2LEVEL_PAGETABLE_MODE
	/* set up an identity-mapped translation table with
	 * strongly ordered memory type and read/write access.
	 */
	for (i=0; i < 4; i++) {
		arm_mmu_map_block(i * GB, i * GB, LPAE_MMU_MEMORY_TYPE_STRONGLY_ORDERED);
	}

	/* set up the translation table base */
	arm_write_ttbr((uint32_t)lpae_tt);

#else
	/* setup L1 table */
	for (i=0; i < 4; i++) {
		ld_tt_l1[i] = ((uint64_t)((uint32_t)&ld_tt_l2[512*i])) | (0x3<<0);
	}
	/* l2 table mapping  */
	for (i=0; i < 2048; i++) {
		ld_tt_l2[i] = i*(2*MB) | (0x1<<10) | (0x3<<8)| (0x1<<0) | LPAE_MMU_MEMORY_TYPE_STRONGLY_ORDERED;
	}

	/* set up the translation table base */
	arm_write_ttbr((uint32_t)ld_tt_l1);
#endif
	/* set up the domain access register */
	arm_write_dacr(0x00000001);

	/* turn on the mmu */
	//arm_write_cr1(arm_read_cr1() | 0x1);
}

void arch_enable_mmu(void)
{
	arm_write_cr1(arm_read_cr1() | 0x1);
}

void arch_disable_mmu(void)
{
	arm_write_cr1(arm_read_cr1() & ~(1<<0));
}
#endif	//MTK_3LEVEL_PAGETABLE
#endif // ARM_WITH_MMU

