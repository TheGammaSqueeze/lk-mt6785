/* Copyright Statement:
*
* This software/firmware and related documentation ("MediaTek Software") are
* protected under relevant copyright laws. The information contained herein
* is confidential and proprietary to MediaTek Inc. and/or its licensors.
* Without the prior written permission of MediaTek inc. and/or its licensors,
* any reproduction, modification, use or disclosure of MediaTek Software,
* and information contained herein, in whole or in part, shall be strictly prohibited.
*/
/* MediaTek Inc. (C) 2015. All rights reserved.
*
* BY OPENING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
* THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
* RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER ON
* AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR NONINFRINGEMENT.
* NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH RESPECT TO THE
* SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY, INCORPORATED IN, OR
* SUPPLIED WITH THE MEDIATEK SOFTWARE, AND RECEIVER AGREES TO LOOK ONLY TO SUCH
* THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO. RECEIVER EXPRESSLY ACKNOWLEDGES
* THAT IT IS RECEIVER'S SOLE RESPONSIBILITY TO OBTAIN FROM ANY THIRD PARTY ALL PROPER LICENSES
* CONTAINED IN MEDIATEK SOFTWARE. MEDIATEK SHALL ALSO NOT BE RESPONSIBLE FOR ANY MEDIATEK
* SOFTWARE RELEASES MADE TO RECEIVER'S SPECIFICATION OR TO CONFORM TO A PARTICULAR
* STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S ENTIRE AND
* CUMULATIVE LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE RELEASED HEREUNDER WILL BE,
* AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE MEDIATEK SOFTWARE AT ISSUE,
* OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE CHARGE PAID BY RECEIVER TO
* MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.
*/

#include <string.h>
#include <stdlib.h>
#include <video_fb.h>
#include <libfdt.h>
#include <platform/disp_drv_platform.h>
#include <target/board.h>
#include <env.h>
#include "lcm_drv.h"
#include <platform/mt_gpt.h>
#include <platform/primary_display.h>
#include <arch/arm/mmu.h>
#include <platform/disp_drv_log.h>
#include "memory_layout.h"
#include <platform/boot_mode.h>
#include <pal_log.h>
#include "../../emu/gba/menu/gba_punch.h"	/* gba_punch_composite (shared w/ host test) */

//#define DFO_DISP
#define FB_LAYER            0
#if defined(MTK_ROUND_CORNER_SUPPORT) && !defined(DISP_HW_RC)
# define BOOT_MENU_LAYER     1
# define TOP_LAYER	2
# define BOTTOM_LAYER	3
#else
# define BOOT_MENU_LAYER     3
#endif

unsigned long long  fb_addr_pa_k    = 0;
static void  *fb_addr_pa      = NULL;
static void  *fb_addr      = NULL;
static void  *logo_db_addr = NULL;
static void  *logo_db_addr_pa = NULL;
static UINT32 fb_size      = 0;
static UINT32 fb_offset_logo = 0; // counter of fb_size
static UINT32 fb_isdirty   = 0;
static UINT32 redoffset_32bit = 1; // ABGR

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
unsigned long long  ext_fb_addr_pa_k    = 0;
static void  *ext_fb_addr_pa      = NULL;
static void  *ext_fb_addr      = NULL;
static void  *ext_logo_db_addr = NULL;
static void  *ext_logo_db_addr_pa = NULL;
static UINT32 ext_fb_size      = 0;
static UINT32 ext_fb_offset_logo = 0; // counter of fb_size
static UINT32 ext_fb_isdirty   = 0;
#endif

extern LCM_PARAMS *lcm_params;

extern void disp_log_enable(int enable);
extern void dbi_log_enable(int enable);
extern void * memset(void *,int,unsigned int);
extern void arch_clean_cache_range(addr_t start, size_t len);
extern UINT32 memory_size(void);

UINT32 mt_disp_get_vram_size(void)
{
	return DISP_GetVRamSize();
}

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
UINT32 mt_disp_get_ext_vram_size(void)
{
	return EXT_DISP_GetVRamSize();
}
#endif

#ifdef DFO_DISP
static disp_dfo_item_t disp_dfo_setting[] = {
	{"LCM_FAKE_WIDTH",  0},
	{"LCM_FAKE_HEIGHT", 0},
	{"DISP_DEBUG_SWITCH",   0}
};

#define MT_DISP_DFO_DEBUG
#ifdef MT_DISP_DFO_DEBUG
#define disp_dfo_printf(string, args...) dprintf(INFO,"[DISP_DFO]"string, ##args)
#else
#define disp_dfo_printf(string, args...) ()
#endif

unsigned int mt_disp_parse_dfo_setting(void)
{
	unsigned int i, j=0 ;
	char tmp[11];
	char *buffer = NULL;
	char *ptr = NULL;

	buffer = (char *)get_env("DFO");
	disp_dfo_printf("env buffer = %s\n", buffer);

	if (buffer != NULL) {
		for (i = 0; i< (sizeof(disp_dfo_setting)/sizeof(disp_dfo_item_t)); i++) {
			j = 0;

			memset((void*)tmp, 0, sizeof(tmp)/sizeof(tmp[0]));

			ptr = strstr(buffer, disp_dfo_setting[i].name);

			if (ptr == NULL) continue;

			disp_dfo_printf("disp_dfo_setting[%d].name = [%s]\n", i, ptr);

			do {} while ((*ptr++) != ',');

			do {tmp[j++] = *ptr++;}
			while (*ptr != ',' && j < sizeof(tmp)/sizeof(tmp[0]));

			disp_dfo_setting[i].value = atoi((const char*)tmp);

			disp_dfo_printf("disp_dfo_setting[%d].name = [%s|%d]\n", i, tmp, disp_dfo_setting[i].value);
		}
	} else {
		disp_dfo_printf("env buffer = NULL\n");
	}

	return 0;
}


int mt_disp_get_dfo_setting(const char *string, unsigned int *value)
{
	char *disp_name;
	int  disp_value;
	unsigned int i = 0;

	if (string == NULL)
		return -1;

	for (i=0; i<(sizeof(disp_dfo_setting)/sizeof(disp_dfo_item_t)); i++) {
		disp_name = disp_dfo_setting[i].name;
		disp_value = disp_dfo_setting[i].value;
		if (!strcmp(disp_name, string)) {
			*value = disp_value;
			disp_dfo_printf("%s = [DEC]%d [HEX]0x%08x\n", disp_name, disp_value, disp_value);
			return 0;
		}
	}

	return 0;
}
#endif

static void _mtkfb_draw_block(unsigned int addr, unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color)
{
	unsigned int i = 0;
	unsigned int j = 0;
	void* start_addr = (void*)(addr+ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4*y+x*4);
	unsigned int pitch = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4;
	unsigned int* line_addr = start_addr;

	for (j=0; j<h; j++) {
		line_addr = start_addr;
		for (i = 0; i<w; i++) {
			line_addr[i] = color;
		}
		start_addr += pitch;
	}
}

static int _mtkfb_internal_test(unsigned int va, unsigned int w, unsigned int h)
{
	/* this is for debug, used in bring up day */
	unsigned int i = 0;
	unsigned int color = 0;
	int _internal_test_block_size = 120;

	for (i = 0; i < w * h / _internal_test_block_size / _internal_test_block_size; i++) {
		color = (i & 0x1) * 0xff;
		color += 0xff000000U;
		_mtkfb_draw_block(va,
				  i % (w / _internal_test_block_size) * _internal_test_block_size,
				  i / (w / _internal_test_block_size) * _internal_test_block_size,
				  _internal_test_block_size, _internal_test_block_size, color);
	}

	primary_display_trigger(1);

	return 0;
}

static int _mtkfb_internal_test2()
{
	/* this is for debug, used in bring up day */
	unsigned int i = 0;
	unsigned int color;
	unsigned int bar_num=16;
	unsigned int bar_size;

	bar_size = fb_size / bar_num;

	for (i = 0; i < bar_num; i++) {
		color = i%2 ? 0 : 0xff;
		memset(fb_addr + i * bar_size, color, bar_size);
	}

	primary_display_trigger(1);

	return 0;
}

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
static void _ext_mtkfb_draw_block(unsigned int addr, unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color)
{
	int i = 0;
	int j = 0;
	void* start_addr = addr+ALIGN_TO(CFG_EXT_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4*y+x*4;
	unsigned int pitch = ALIGN_TO(CFG_EXT_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4;
	unsigned int* line_addr = start_addr;

	for (j=0; j<h; j++) {
		line_addr = start_addr;
		for (i = 0; i<w; i++) {
			line_addr[i] = color;
	}
		start_addr += pitch;
	}
}

static int _ext_mtkfb_internal_test(unsigned int va, unsigned int w, unsigned int h)
{
	/* this is for debug, used in bring up day */
	unsigned int i = 0;
	unsigned int color = 0;
	int _internal_test_block_size = 120;

	for (i = 0; i < w * h / _internal_test_block_size / _internal_test_block_size; i++) {
		color = (i & 0x1) * 0xff;
		color += 0xff000000U;
		_mtkfb_draw_block(va,
			i % (w / _internal_test_block_size) * _internal_test_block_size,
			i / (w / _internal_test_block_size) * _internal_test_block_size,
			_internal_test_block_size, _internal_test_block_size, color);
	}

	arch_clean_cache_range((unsigned int)ext_fb_addr, EXT_DISP_GetFBRamSize());
	external_display_trigger(1);

	return 0;
}

static int _ext_mtkfb_internal_test2()
{
       /* this is for debug, used in bring up day */
       unsigned int i = 0;
       unsigned int color;
       int bar_num=16;
       unsigned int bar_size;

       bar_size = ext_fb_size / bar_num;

       for (i = 0; i < bar_num; i++) {
               color = i%2 ? 0 : 0xff;
               memset(ext_fb_addr + i * bar_size, color, bar_size);
       }
       arch_clean_cache_range((unsigned int)ext_fb_addr, EXT_DISP_GetFBRamSize());
       external_display_trigger(1);

       return 0;
}
#endif
#if defined(MTK_ROUND_CORNER_SUPPORT) && !defined(DISP_HW_RC)
void assemble_image(void *dst, void *left, void *right, int h, int picture_w, int w)
{
	int i = 0;
	for (i = 0; i < h; i++) {
		memcpy(dst+picture_w*i*2, left+2*w*i, w*2);
		memcpy(dst+(picture_w*(i+1)-w)*2, right+2*w*i, w*2);
	}
}

int round_corner_init(unsigned int *top_pa, unsigned int *bottom_pa, LCM_ROUND_CORNER *rc_params)
{
	unsigned int h = rc_params->h;
	unsigned int w = rc_params->w;
	unsigned int pitch = DISP_GetScreenWidth();
	unsigned char *left_top = rc_params->lt_addr;
	unsigned char *left_bottom = rc_params->lb_addr;
	unsigned char *right_top = rc_params->rt_addr;
	unsigned char *right_bottom = rc_params->rb_addr;
	unsigned int buf_size = 0;

	static void *top_addr_va = NULL;
	static void *bottom_addr_va = NULL;

	if (h == 0 || w == 0 || left_top == NULL || left_bottom == NULL
		|| right_top == NULL || right_bottom == NULL) {
		dprintf(CRITICAL, "the round corner params is invalid, please check the lcm config\n");
		return -1;
	}

	buf_size = h * pitch * 2;
	*top_pa = (unsigned int)(fb_addr_pa + DISP_GetVRamSize() - 2 * buf_size);
	top_addr_va = fb_addr + DISP_GetVRamSize() - 2 * buf_size;
	dprintf(INFO,"top_addr_va: 0x%08x, top_pa: 0x%08x\n", (unsigned int)top_addr_va, *top_pa);

	*bottom_pa = (unsigned int)(fb_addr_pa + DISP_GetVRamSize() - buf_size);
	bottom_addr_va = fb_addr + DISP_GetVRamSize() - buf_size;
	dprintf(INFO,"bottom_addr_va: 0x%08x, bottom_pa: 0x%08x\n", (unsigned int)bottom_addr_va, *bottom_pa);

	assemble_image((void *)top_addr_va, (void *)left_top, (void *)right_top, h, pitch, w);
	assemble_image((void *)bottom_addr_va, (void *)left_bottom, (void *)right_bottom, h, pitch, w);

	return 0;
}
#endif

void mt_disp_init(void *lcdbase)
{
	UINT32 boot_mode_addr = 0;

#if defined(MTK_ROUND_CORNER_SUPPORT) && !defined(DISP_HW_RC)
	int ret = -1;
	unsigned int top_addr_pa = 0;
	unsigned int bottom_addr_pa = 0;
	LCM_ROUND_CORNER *round_corner = primary_display_get_corner_params();
#endif
	/// fb base pa and va
	fb_addr_pa_k = arm_mmu_va2pa((unsigned int)lcdbase);

	fb_addr_pa   = (void *)(unsigned int)(fb_addr_pa_k & 0xffffffffull);
	fb_addr      = lcdbase;

	dprintf(0,"fb_va: 0x%08x, fb_pa: 0x%08x, fb_pa_k: 0x%llx\n", (unsigned int)fb_addr, (unsigned int)fb_addr_pa, fb_addr_pa_k);

	fb_size = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT) * ALIGN_TO(CFG_DISPLAY_HEIGHT, MTK_FB_ALIGNMENT) * CFG_DISPLAY_BPP / 8;
	// pa;
	boot_mode_addr = ((UINT32)fb_addr_pa + fb_size);

	logo_db_addr_pa = (void *)(u32)mblock_reserve_ext(&g_boot_arg->mblock_info,
		LK_LOGO_MAX_SIZE, PAGE_SIZE, 0x80000000, 0, "logo_db_addr_pa");

	if (!logo_db_addr_pa) {
		pal_log_err("Warning! logo_db_addr_pa is not taken from mb\n");
		assert(0);
	}

	// va;
	logo_db_addr = logo_db_addr_pa;

	fb_offset_logo = 0;

	primary_display_init(NULL);
	memset((void*)lcdbase, 0x0, DISP_GetVRamSize());

	disp_input_config input;

#if defined(MTK_ROUND_CORNER_SUPPORT) && !defined(DISP_HW_RC)
	if (round_corner->round_corner_en) {
		if (round_corner != NULL)
			ret = round_corner_init(&top_addr_pa, &bottom_addr_pa, round_corner);

		if (ret == 0) {
			memset(&input, 0, sizeof(input));
			input.layer     = TOP_LAYER;
			input.layer_en  = 1;
			input.fmt       = eRGBA4444;
			input.addr      = top_addr_pa;
			input.src_x     = 0;
			input.src_y     = 0;
			input.src_w     = CFG_DISPLAY_WIDTH;
			input.src_h     = round_corner->h;
			input.src_pitch = CFG_DISPLAY_WIDTH*2;
			input.dst_x     = 0;
			input.dst_y     = 0;
			input.dst_w     = CFG_DISPLAY_WIDTH;
			input.dst_h     = round_corner->h;
			input.aen       = 1;
			input.alpha     = 0xff;

			primary_display_config_input(&input);

			memset(&input, 0, sizeof(input));
			input.layer     = BOTTOM_LAYER;
			input.layer_en  = 1;
			input.fmt       = eRGBA4444;
			input.addr      = bottom_addr_pa;
			input.src_x     = 0;
			input.src_y     = 0;
			input.src_w     = CFG_DISPLAY_WIDTH;
			input.src_h     = round_corner->h;
			input.src_pitch = CFG_DISPLAY_WIDTH*2;
			input.dst_x     = 0;
			input.dst_y     = CFG_DISPLAY_HEIGHT-round_corner->h;
			input.dst_w     = CFG_DISPLAY_WIDTH;
			input.dst_h     = round_corner->h;
			input.aen       = 1;
			input.alpha     = 0xff;

			primary_display_config_input(&input);
		}
	}
#endif
	memset(&input, 0, sizeof(disp_input_config));
	input.layer     = BOOT_MENU_LAYER;
	input.layer_en  = 1;
	input.fmt       = redoffset_32bit ? eBGRA8888 : eRGBA8888;
	input.addr      = boot_mode_addr;
	input.src_x     = 0;
	input.src_y     = 0;
	input.src_w     = CFG_DISPLAY_WIDTH;
	input.src_h     = CFG_DISPLAY_HEIGHT;
	input.src_pitch = CFG_DISPLAY_WIDTH*4;
	input.dst_x     = 0;
	input.dst_y     = 0;
	input.dst_w     = CFG_DISPLAY_WIDTH;
	input.dst_h     = CFG_DISPLAY_HEIGHT;
	input.aen       = 1;
	input.alpha     = 0xff;

	primary_display_config_input(&input);

	memset(&input, 0, sizeof(disp_input_config));
	input.layer     = FB_LAYER;
	input.layer_en  = 1;
	input.fmt       = redoffset_32bit ? eBGRA8888 : eRGBA8888;
	input.addr      = (unsigned int)fb_addr_pa;
	input.src_x     = 0;
	input.src_y     = 0;
	input.src_w     = CFG_DISPLAY_WIDTH;
	input.src_h     = CFG_DISPLAY_HEIGHT;
	input.src_pitch = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4;
	input.dst_x     = 0;
	input.dst_y     = 0;
	input.dst_w     = CFG_DISPLAY_WIDTH;
	input.dst_h     = CFG_DISPLAY_HEIGHT;

	input.aen       = 1;
	input.alpha     = 0xff;
	primary_display_config_input(&input);

#if 0
	/* debug for bringup */
	dprintf(CRITICAL, "display show background\n");
	primary_display_trigger(TRUE);
	mdelay(100);
	primary_display_diagnose();
/*
	while(1) {
		primary_display_trigger(TRUE);
		if (!primary_display_is_video_mode()) {
			dprintf(CRITICAL,"cmd mode trigger wait\n");
			mdelay(100);
		}
		primary_display_diagnose();
	}
*/
	dprintf(CRITICAL, "display internal test\n");
	_mtkfb_internal_test2(fb_addr, CFG_DISPLAY_WIDTH, CFG_DISPLAY_HEIGHT);
	mdelay(100);
	primary_display_diagnose();
#endif

#ifdef DFO_DISP
	unsigned int lcm_fake_width = 0;
	unsigned int lcm_fake_height = 0;

	mt_disp_parse_dfo_setting();

	if ((0 == mt_disp_get_dfo_setting("LCM_FAKE_WIDTH", &lcm_fake_width)) && (0 == mt_disp_get_dfo_setting("LCM_FAKE_HEIGHT", &lcm_fake_height))) {
		if (DISP_STATUS_OK != DISP_Change_LCM_Resolution(lcm_fake_width, lcm_fake_height)) {
			dprintf(INFO,"[DISP_DFO]WARNING!!! Change LCM Resolution FAILED!!!\n");
		}
	}
#endif

	DISPMSG("mt_disp_init() done\n");

}

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
void mt_ext_disp_init(void *lcdbase)
{
	UINT32 ext_boot_mode_addr = 0;
	/// fb base pa and va
	ext_fb_addr_pa_k = arm_mmu_va2pa(lcdbase);

	ext_fb_addr_pa   = ext_fb_addr_pa_k & 0xffffffffull;
	ext_fb_addr      = lcdbase;

	dprintf(0,"ext_fb_va: 0x%08x, ext_fb_pa: 0x%08x, ext_fb_pa_k: 0x%llx\n", ext_fb_addr, ext_fb_addr_pa, ext_fb_addr_pa_k);

	ext_fb_size = ALIGN_TO(CFG_EXT_DISPLAY_WIDTH, MTK_FB_ALIGNMENT) * ALIGN_TO(CFG_EXT_DISPLAY_HEIGHT, MTK_FB_ALIGNMENT) * CFG_EXT_DISPLAY_BPP / 8;
	// pa;
	ext_boot_mode_addr = ((UINT32)ext_fb_addr_pa + ext_fb_size);
	//ext_logo_db_addr_pa = (void *)((UINT32) SCRATCH_ADDR + SCRATCH_SIZE);

	// va;
	//ext_logo_db_addr = (void *)((UINT32) SCRATCH_ADDR + SCRATCH_SIZE);

	ext_fb_offset_logo = 0;

	external_display_init(NULL);

	memset((void*)lcdbase, 0x0, EXT_DISP_GetVRamSize());

	disp_input_config ext_input;

	memset(&ext_input, 0, sizeof(disp_input_config));
	ext_input.layer     = BOOT_MENU_LAYER;
	ext_input.layer_en  = 1;
	ext_input.fmt       = redoffset_32bit ? eBGRA8888 : eRGBA8888;
	ext_input.addr      = ext_boot_mode_addr;
	ext_input.src_x     = 0;
	ext_input.src_y     = 0;
	ext_input.src_w     = CFG_EXT_DISPLAY_WIDTH;
	ext_input.src_h     = CFG_EXT_DISPLAY_HEIGHT;
	ext_input.src_pitch = CFG_EXT_DISPLAY_WIDTH*4;
	ext_input.dst_x     = 0;
	ext_input.aen       = 1;
	ext_input.alpha     = 0xff;
	external_display_config_input(&ext_input);

	memset(&ext_input, 0, sizeof(disp_input_config));
	ext_input.layer         = FB_LAYER;
	ext_input.layer_en      = 1;
	ext_input.fmt           = redoffset_32bit ? eBGRA8888 : eRGBA8888;
	ext_input.addr          = ext_fb_addr_pa;
	ext_input.src_x         = 0;
	ext_input.src_y         = 0;
	ext_input.src_w         = CFG_EXT_DISPLAY_WIDTH;
	ext_input.src_h         = CFG_EXT_DISPLAY_HEIGHT;
	ext_input.src_pitch = ALIGN_TO(CFG_EXT_DISPLAY_WIDTH, MTK_FB_ALIGNMENT)*4;
	ext_input.dst_x         = 0;
	ext_input.dst_y         = 0;
	ext_input.dst_w         = CFG_EXT_DISPLAY_WIDTH;
	ext_input.dst_h         = CFG_EXT_DISPLAY_HEIGHT;

	ext_input.aen           = 1;
	ext_input.alpha         = 0xff;
	external_display_config_input(&ext_input);

	/* external internal test */
#if 1
	dprintf(CRITICAL, "external display internal test\n");
	/* _ext_mtkfb_internal_test(ext_fb_addr, CFG_EXT_DISPLAY_WIDTH, CFG_EXT_DISPLAY_HEIGHT); */
	_ext_mtkfb_internal_test2();
#endif
	DISPMSG("mt_ext_disp_init() done\n");
}

void mt_ext_disp_deinit(void)
{
	external_display_suspend();
}
#endif

void mt_disp_power(BOOL on)
{
	dprintf(0,"mt_disp_power %d\n",on);
	return;
}

void mt_free_logo_from_mblock(void)
{
	if (logo_db_addr_pa) {
		mblock_create(&g_boot_arg->mblock_info,
			&g_boot_arg->orig_dram_info,
			(u64)(unsigned long)logo_db_addr_pa, (u64)(LK_LOGO_MAX_SIZE));

		logo_db_addr_pa = NULL;
		logo_db_addr = NULL;
	}
}

void* mt_get_logo_db_addr(void)
{
	dprintf(0,"mt_get_logo_db_addr: 0x%08x\n",(unsigned int)logo_db_addr);
	return logo_db_addr;
}

void* mt_get_logo_db_addr_pa(void)
{
	dprintf(0,"mt_get_logo_db_addr_pa: 0x%08x\n",(unsigned int)logo_db_addr_pa);
	return logo_db_addr_pa;
}

void* mt_get_fb_addr(void)
{
	fb_isdirty = 1;
	return (void*)((UINT32)fb_addr + fb_offset_logo * fb_size);
}

void* mt_get_tempfb_addr(void)
{
	//use offset = 2 as tempfb for decompress logo
	dprintf(0,"mt_get_tempfb_addr: 0x%08x ,fb_addr 0x%08x\n",(unsigned int)((UINT32)fb_addr + 2*fb_size),(unsigned int)fb_addr);
	return (void*)((UINT32)fb_addr + 2*fb_size);
}

UINT32 mt_get_fb_size(void)
{
	return fb_size;
}

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
void* mt_get_ext_logo_db_addr(void)
{
	dprintf(0,"mt_get_ext_logo_db_addr: 0x%08x\n",ext_logo_db_addr);
	return ext_logo_db_addr;
}

void* mt_get_ext_logo_db_addr_pa(void)
{
	dprintf(0,"mt_get_ext_logo_db_addr_pa: 0x%08x\n",ext_logo_db_addr_pa);
	return ext_logo_db_addr_pa;
}

void* mt_get_ext_fb_addr(void)
{
	ext_fb_isdirty = 1;
	return (void*)((UINT32)ext_fb_addr + ext_fb_offset_logo * ext_fb_size);
}

void* mt_get_ext_tempfb_addr(void)
{
	//use offset = 2 as tempfb for decompress logo
	dprintf(0,"mt_get_ext_tempfb_addr: 0x%08x ,ext_fb_addr 0x%08x\n",(void*)((UINT32)ext_fb_addr + 2*ext_fb_size),(void*)ext_fb_addr);
	return (void*)((UINT32)ext_fb_addr + 2 * ext_fb_size);
}

INT32 mt_get_ext_fb_size(void)
{
	return ext_fb_size;
}
#endif

void mt_disp_update(UINT32 x, UINT32 y, UINT32 width, UINT32 height)
{
	arch_clean_cache_range((unsigned int)fb_addr, DISP_GetFBRamSize());
	primary_display_trigger(TRUE);
}

#ifdef AYANEO_RAINBOW_BOOT
#include <kernel/thread.h>
#include <platform.h>			/* current_time() */

#include <part_interface.h>		/* partition_read() */

/* Playback frame rate. */
#ifndef AYANEO_ANIM_FPS
#define AYANEO_ANIM_FPS 30
#endif
/*
 * Safety cap (ms) for how long boot may wait at the handoff for the animation
 * to finish playing. The animation is ~5.5s; boot normally reaches the handoff
 * sooner, so it holds (the player keeps running) until the animation completes
 * or this cap elapses. This is bounded, added boot latency.
 */
#ifndef AYANEO_ANIM_MAX_MS
#define AYANEO_ANIM_MAX_MS 8000
#endif

/*
 * Compressed animation blob lives in the unused inactive boot slot, boot_b
 * (33 MB, raw NORMAL_ROM). The logo partition's DA verification buffer is too
 * small (~1.5 MB) for our blob, but boot_b is raw-flashed with a large buffer
 * and no MTK-header type check, so the full-quality blob fits. The device runs
 * slot A (pinned) and skips verification, so boot_b is never selected.
 * The player still auto-detects a raw (offset 0) or MTK-header-wrapped
 * (offset AYANEO_ANIM_HDR) layout.
 */
#define AYANEO_ANIM_PART      "boot_b"
#define AYANEO_ANIM_HDR       512
#define AYANEO_ANIM_MAGIC     0x31414247u	/* 'GBA1' little-endian */
/* max decoded source frame we support (1280x720 RGB565); source is streamed */
#define AYANEO_ANIM_RGBMAX    (1280 * 720 * 2)
/* code-driven fade-out (triggered when boot is ready, not baked into the blob) */
#define AYANEO_FADE_STEPS     8
#define AYANEO_FADE_STEP_MS   28

extern int zunzip(unsigned char *src, unsigned long *lenp, void *dst,
		  int dstlen, int offset);
extern void *memmove(void *dest, const void *src, unsigned int n);

/*
 * AYANEO experiment: scrolling diagonal rainbow over the whole panel during LK.
 * Runs on its own LK thread so it keeps animating while the main thread carries
 * on booting; it is stopped just before the kernel handoff. Two scan-out
 * buffers (fb offset 0 and the unused logo tempfb at offset 2) are alternated
 * because in DSI video mode a bare trigger early-returns (never pushes a new
 * frame) and primary_display_config_input() with an unchanged address is a
 * no-op - handing the OVL a different address each frame forces a re-latch.
 * config_input() blocks on FRAME_DONE, which paces the animation to vsync and
 * yields the CPU to the boot thread.
 */
extern void mt65xx_backlight_on(void);
extern void thread_set_priority(int priority);
#ifdef AYANEO_BOOT_AUDIO
extern void ayaneo_boot_audio_start(void);
extern void ayaneo_boot_audio_stop(void);
#endif

/* ---- backlight brightness + on-screen slider (volume/brightness) ---- */
extern int mt65xx_leds_brightness_set(int type, int level);	/* mt_leds.c */
extern void ayaneo_apply_persisted_brightness(void);		/* ayaneo_audio.c */

/* Drive the LCD backlight to 'level' (0-255). Called from ayaneo_audio.c which
 * owns the persisted/runtime brightness value. type 6 = MT65XX_LED_TYPE_LCD. */
void ayaneo_apply_backlight(int level)
{
	mt65xx_leds_brightness_set(6, level);
}

/* OSD slider shown briefly when the user changes volume/brightness. Set from the
 * emulator's input poll (gbc_driver.c), drawn by ayaneo_gbc_show_frame(). */
static volatile int s_osd_kind;			/* 0 none, 1 volume, 2 brightness */
static volatile int s_osd_pct;			/* 0-100 */
static volatile unsigned int s_osd_until_ms;	/* current_time() deadline */
void ayaneo_gbc_osd_show(int kind, int pct)
{
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	s_osd_kind = kind;
	s_osd_pct = pct;
	s_osd_until_ms = (unsigned int)current_time() + 1500;	/* ~1.5 s */
}

static volatile int s_rainbow_stop;
/* Default 1 ("nothing running") so video_rainbow_boot_stop() returns at once when
 * the animation was never started (e.g. the charger-boot path skips it) instead
 * of sitting in its ~9 s wait-for-animation timeout. Set to 0 while the animation
 * thread is actually running; back to 1 when it exits. */
static volatile int s_rainbow_exited = 1;
static volatile int s_anim_complete;
static volatile int s_fade_request;	/* boot is ready -> fade out and hand off */
static volatile unsigned int s_rainbow_start_ms;

/* nearest-neighbour source-x lookup so scaling is a table read, not a divide */
static unsigned short s_sxmap[2048];

/* unaligned little-endian reads from the blob (avoid alignment faults) */
static unsigned int rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned int rd32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Present display buffer at physical 'pa'. Alternating pa each frame forces the
 * OVL to re-latch (config_input with an unchanged address is a no-op in video
 * mode); a bare trigger alone never pushes a new frame. */
/* When set, primary_display_config_input() skips its frame-done wait so presents
 * are non-blocking (used by the GBA benchmark to run uncapped). */
int ayaneo_present_skip_framedone = 0;

static void ayaneo_present(unsigned int pa, unsigned int W, unsigned int H,
			   unsigned int pitch_w)
{
	disp_input_config input;

	memset(&input, 0, sizeof(input));
	input.layer     = FB_LAYER;
	input.layer_en  = 1;
	input.fmt       = redoffset_32bit ? eBGRA8888 : eRGBA8888;
	input.addr      = pa;
	input.src_w     = W;
	input.src_h     = H;
	input.src_pitch = pitch_w * 4;
	input.dst_w     = W;
	input.dst_h     = H;
	input.aen       = 1;
	input.alpha     = 0xff;
	primary_display_config_input(&input);
	primary_display_trigger(TRUE);
}

#if defined(AYANEO_GBC) || defined(AYANEO_GBA)
#include <video_font.h>		/* mtk_vdo_fntdata: 8x16 ASCII font */

/*
 * Display a 160x144 RGB565 Game Boy frame from the emulator: integer 6x scale
 * (960x864) centred on the panel with black borders. Double-buffered like the
 * animation so the OVL re-latches in DSI video mode. The static black borders
 * are cleared once; per frame only the game area is blitted.
 */
#if defined(AYANEO_GBA)
/* GBA: 240x160 native, integer 5x -> 1200x800 centred on the 1280x960 panel. The
 * game runs at this fast, unclipped scale; the BIOS-logo intro uses a separate
 * 6x fill-height path (ayaneo_gba_show_intro_frame). */
#define GBC_SRC_W	240
#define GBC_SRC_H	160
#define GBC_SCALE	5
#else
/* GBC: 160x144 native, integer 6x -> 960x864. */
#define GBC_SRC_W	160
#define GBC_SRC_H	144
#define GBC_SCALE	6
#endif

/* Shared double-buffer flip index for the game frame and the menu canvas (they
 * are never rendered at the same time - the menu pauses the emulator). */
static int s_fb_flip;

/* ---- logical screen and rotation ------------------------------------------------------------
 * The Konkr Pocket Advance panel is scanned 640 wide x 960 tall and mounted landscape, so the LK
 * frame buffer (CFG_DISPLAY_WIDTH x CFG_DISPLAY_HEIGHT) is portrait while everything the user sees
 * is a 960x640 landscape picture. All emulator and menu geometry below is expressed on the LOGICAL
 * screen (ayaneo_screen_w/h); the few places that hand pixels to the hardware rotate them into the
 * physical buffer: native emulator frames are turned before the resizer upscales them (cheap, a
 * few tens of thousands of pixels), and the full-screen ARGB canvas the menu / dialogs / transitions
 * draw into is turned once per present. On the pairmini the panel is landscape, AYANEO_ROT is 0 and
 * every helper collapses to the identity, so that build is unchanged.
 *
 * Turn direction: logical (x, y) -> physical (PW - 1 - y, x) is a clockwise quarter turn, the
 * alternative is (y, PH - 1 - x). Which one the glass needs is decided on the device (oem rot:N
 * flips g_rot_ccw live); the default follows the stock LK's "90". */
#ifdef KONKR_KPA
#define AYANEO_ROT 1
/* Scratch in the WB-mapped DRAM window, laid out around the fixed transition buffers (see the
 * map in gba_snes_menu.c). With the 640x960x4 frame buffer (0x258000) these ranges are disjoint from
 * the snapshot (0x54000000+fb), reveal (0x55000000+fb), boxart (0x55500000..0x557E4000), freeze
 * (0x55800000), game-full / overlay / RSZ_BUF1 (0x55900000+fb) and fct (0x55E00000..) regions. The
 * 4:3 warped-wallpaper cache that starts at 0x54000000 is never built on this board (3:2 screen). */
/* The menu, dialogs and transitions draw on a canvas the size of the logical 960x640 screen
 * (the menu engine renders at its native 0.75 scale); ayaneo_canvas_present turns it into the
 * portrait frame buffer. (Until build33 this was a 1280x853 virtual canvas downscaled by 3/4
 * on every present, which cost about 10 ms real per frame.) The canvas and the rotated-native-frame scratch share one region: the canvas is only drawn
 * while no emulator frame is being presented (menu, dialogs, transitions) and vice versa. The
 * downscale temp shares the overlay paint target for the same reason (menu time vs game time).
 * 0x54800000 sits above the (3:2 sized) warped-wallpaper cache and below the relocated card
 * tile cache (gba_snes_menu.c, KONKR_KPA map). */
/* The canvas is the logical (landscape) screen itself, 960x640 on the Konkr: the menu renders
 * at its native 0.75 scale (snes_target_fit) and the present is a plain quarter turn. The
 * region is sized for the earlier 1280x853 virtual canvas, so anything up to that still fits. */
#define ROT_CANVAS_W    ((unsigned int)CFG_DISPLAY_HEIGHT)
#define ROT_CANVAS_H    ((unsigned int)CFG_DISPLAY_WIDTH)
#define ROT_CANVAS_PA   0x54800000u   /* up to 1280x853x4 = 0x42A400, ends 0x54C2A400 */
#define ROT_565_PA      0x54800000u   /* rotated native frame, up to 1280x720 RGB565 (0x1C2000) */
#define ROT_OVERLAY_PA  0x55B80000u   /* logical ARGB paint target for the game overlay (0x258000) */
#define ROT_LOGICAL_PA  0x55B80000u   /* logical 960x640 temp of the downscaled canvas */
#define ROT_565_MAX_PX  (1280u * 720u)
#else
#define AYANEO_ROT 0
#endif

/* Size of the canvas handed out by ayaneo_canvas_back (the panel on the pairmini, the virtual
 * 1280x853 on the Konkr). */
void ayaneo_canvas_dims(unsigned int *cw, unsigned int *ch)
{
#if AYANEO_ROT
	*cw = ROT_CANVAS_W; *ch = ROT_CANVAS_H;
#else
	*cw = CFG_DISPLAY_WIDTH; *ch = CFG_DISPLAY_HEIGHT;
#endif
}
volatile int g_rot_ccw;   /* 0 = clockwise quarter turn (default), 1 = counter clockwise */
volatile unsigned int g_dbg_present_us[2];   /* canvas present cost: [0] downscale, [1] turn (us) */
volatile unsigned int g_dbg_pace_waits;      /* refresh edges waited by the last present (Konkr: 2 = on cadence) */
volatile unsigned int g_dbg_pace_dt_us;      /* interval between the last two presents (us) */
volatile unsigned int g_dbg_pace_timeouts;   /* frame-done waits that timed out (should stay 0) */
volatile unsigned int g_dbg_pace_edge_us;    /* duration of the last single edge wait (us) */
volatile unsigned int g_dbg_present_fps;     /* paced presents in the last second */
#if AYANEO_ROT
/* Deadline pacing. A frame must be shown for exactly g_vsync_div refreshes (1 on the Konkr:
 * the panel is 60 Hz class, the earlier "120 Hz" reading came from GPT4 ticking at 6.5 MHz). Waiting "frame-done, then a vsync, then one more" costs two or three refreshes
 * depending on where the render finished, which alternates 16.7 / 25 ms and judders the menu.
 * Instead: present without the frame-done block, then wait vsync edges only until div periods
 * have passed since the previous frame's edge. Steady 2 refreshes whenever render + present
 * fit in the budget; a late frame slips one refresh, never two. */
static unsigned int s_pace_edge;
/* Did the overlay really switch to the buffer presented one frame ago? Checked right after the
 * frame end edge against the live OVL0_2L L0 address (the sampler in oem latch starves behind
 * the render thread and under-counts). Only the canvas path arms s_pace_expect. */
static unsigned int s_pace_expect;
volatile unsigned int g_dbg_latch_hit, g_dbg_latch_miss;
int ayaneo_rdma_wait_frame_end(unsigned int timeout_us);   /* ddp_rdma.c */
static unsigned int s_pace_next;   /* time paced mode: deadline of the frame being presented */
extern void ayaneo_idle_until(unsigned target_tick);   /* ayaneo_volume_input.c */
/* Frame delay. A core reads its buttons at the top of its loop, emulates, presents, and the
 * present waits in rot_pace for the refresh edge: the time left over (often half the refresh or
 * more) used to sit between the button read and the picture. ayaneo_frame_delay(), called by each
 * core just before it reads its buttons, sleeps that leftover away first, so the buttons are read
 * as late as the frame can still make its edge. The room it leaves is learnt here: rot_pace
 * measures each frame's work from that call to the present, and the delay keeps the worst of the
 * last FD_HIST frames plus a margin. A delayed frame that still reaches the present after its
 * edge (it would stay up a refresh longer) turns the delay off for two seconds and widens the
 * margin. Only with whole refresh pacing (every core but a PAL one); fast forward and rewind fill
 * the refresh, so the delay falls to nothing by itself while they run. */
#define FD_HIST 32
extern unsigned int gpt4_get_current_tick(void);
static unsigned int s_fd_start;            /* tick this frame's work started, 0 = none to measure */
static int s_fd_delayed;                   /* this frame's button read was delayed */
static unsigned int s_fd_work[FD_HIST], s_fd_n;
static unsigned int s_fd_margin = 2u * 13000u;
static unsigned int s_fd_off_until;
static int s_fd_off;
static int s_fd_whole;                     /* the last present paced whole refreshes */
static unsigned int s_fd_period;           /* ticks between two presents' edges */
int g_fd_enable = 1;                       /* oem fdelay:0|1 */
volatile unsigned int g_dbg_fd_us, g_dbg_fd_work_us, g_dbg_fd_misses, g_dbg_fd_off;
/* totals since oem fdelay last read them: frames, delay and work (us), worst work (us) */
volatile unsigned int g_dbg_fd_frames, g_dbg_fd_sum_us, g_dbg_fd_sum_work_us, g_dbg_fd_max_work_us;

static void fd_present_entry(unsigned int now)
{
	unsigned int work, edge = s_pace_edge + s_fd_period;
	if (!s_fd_start) return;
	work = now - s_fd_start;
	s_fd_start = 0;
	if (work < 2u * s_fd_period) {
		s_fd_work[s_fd_n++ % FD_HIST] = work;
		g_dbg_fd_work_us = work / 13u;
		g_dbg_fd_frames++;
		g_dbg_fd_sum_us += g_dbg_fd_us;
		g_dbg_fd_sum_work_us += work / 13u;
		if (work / 13u > g_dbg_fd_max_work_us) g_dbg_fd_max_work_us = work / 13u;
	}
	if (!s_fd_delayed || !s_fd_whole) return;
	if ((int)(now - edge) > 0) {               /* the edge has gone: this frame slips a refresh */
		g_dbg_fd_misses++;
		s_fd_off = 1;
		s_fd_off_until = now + 2u * 13000000u;
		if (s_fd_margin < 8u * 13000u) s_fd_margin += 13000u;
	} else if (s_fd_margin > 2u * 13000u) {
		s_fd_margin -= 13u;                    /* back by 1 us a frame while frames land */
	}
}

/* The ticks left until the next present's edge, or 0 when that is not known (PAL pacing, a
 * stale edge). Fast forward budgets its extra frames to it. */
unsigned int ayaneo_pace_ticks_left(void)
{
	unsigned int now = gpt4_get_current_tick(), edge = s_pace_edge + s_fd_period;
	if (!s_fd_whole || !s_fd_period) return 0;
	if ((int)(edge - now) <= 0 || (int)(edge - now) > (int)(2u * s_fd_period)) return 0;
	return edge - now;
}

void ayaneo_frame_delay(void)
{
	unsigned int now = gpt4_get_current_tick(), left, est = 0, need, i;
	s_fd_delayed = 0;
	if (s_fd_off && (int)(now - s_fd_off_until) >= 0) s_fd_off = 0;
	g_dbg_fd_off = s_fd_off;
	left = ayaneo_pace_ticks_left();
	if (g_fd_enable && !s_fd_off && left && s_fd_n >= FD_HIST) {
		for (i = 0; i < FD_HIST; i++) if (s_fd_work[i] > est) est = s_fd_work[i];
		need = est + est / 8u + s_fd_margin;
		/* Sleep in 1 ms ticks (each returns within about 2 ms) while more than that is left, then
		 * yield until the wake time: the yields keep threads of the same priority (fastboot) served
		 * without the 1 ms tick's error. */
		if (left > need + 13000u / 2u) {
			unsigned int wake = now + (left - need);
			while ((int)(wake - gpt4_get_current_tick()) > (int)(5u * 13000u / 2u))
				thread_sleep(1);
			while ((int)(wake - gpt4_get_current_tick()) > 0)
				thread_yield();
			s_fd_delayed = 1;
		}
	}
	s_fd_start = gpt4_get_current_tick();
	if (!s_fd_start) s_fd_start = 1;
	g_dbg_fd_us = (s_fd_start - now) / 13u;
}

static void rot_pace(void)
{
	extern volatile unsigned int g_vsync_div;
	extern unsigned int ayaneo_dsi_refresh_milli(void);
	extern unsigned int gpt4_get_current_tick(void);
	extern volatile unsigned int g_pace_fps_milli;
	unsigned int r = ayaneo_dsi_refresh_milli();
	unsigned int period = r ? (unsigned int)(13000000000ULL / r) : 108800u;   /* 13 MHz ticks per refresh */
	unsigned int div = g_vsync_div ? g_vsync_div : 1u;
	unsigned int fps = g_pace_fps_milli ? g_pace_fps_milli : 59727u;
	unsigned int frame = (unsigned int)(13000000000ULL / fps);                /* ticks per core frame */
	unsigned int target, now, n = 0;
	/* Pace by the 13 MHz clock to the core's own frame period. The OVL latches a presented
	 * buffer at the next refresh, so with the panel tuned to 2x the core rate every frame lands
	 * on two refreshes and the phase stays put; a core the porch cannot follow (PAL) gets 2/3
	 * holds averaging its rate. The display interrupt flags were tried as the clock first and
	 * failed three ways on this board (DSI FRAME_DONE never fires in video mode, dpmgr's event
	 * is not initialised on the LK path, RDMA0 FRAME_END did not behave as a per frame edge),
	 * so the wall clock is the one reference that cannot lie. thread_yield keeps the fastboot
	 * USB thread served while we wait. */
	/* Returns right after an RDMA0 FRAME_END edge (the one safe phase to write the layer config,
	 * measured on the device: a config written elsewhere in the refresh is dropped about half
	 * the time). Integer cadence (panel within 1% of div x the core rate): take edges from the
	 * previous present's edge until div periods have passed, so the frame period is exactly div
	 * refreshes and the render may use all of it. Otherwise (PAL) a time deadline picks whether
	 * the frame holds 2 or 3 refreshes, averaging the core rate. */
	/* After a gap (a dark charging screen, a long load) the last edge is stale: a 13 MHz
	 * difference older than ~165 s reads positive again and the idle sleep would wait minutes.
	 * Rebase on the clock and take the next edge instead. */
	now = gpt4_get_current_tick();
	fd_present_entry(now);
	if (now - s_pace_edge > 4u * period) s_pace_edge = now - period * div;
	s_fd_whole = frame > period * div - period / 100u && frame < period * div + period / 100u;
	s_fd_period = period * div;
	if (s_fd_whole) {
		target = s_pace_edge + period * div - period / 4u;
		s_pace_next = 0;
		ayaneo_idle_until(s_pace_edge + period * div);   /* sleep until a few ms before the edge we want */
	} else {
		now = gpt4_get_current_tick();
		if (s_pace_next == 0u || (int)(now - s_pace_next) > (int)(2u * frame) || (int)(s_pace_next - now) > (int)(2u * frame))
			s_pace_next = now;
		s_pace_next += frame;
		target = s_pace_next - period / 2u;   /* the first edge at or after this is the one */
		ayaneo_idle_until(target);
	}
	do {
		if (!ayaneo_rdma_wait_frame_end(30000u)) g_dbg_pace_timeouts++;
		now = gpt4_get_current_tick();
		n++;
	} while ((int)(now - target) < 0 && n < 8u);
	{ extern void disp_pwm_strobe_tick(void); disp_pwm_strobe_tick(); }   /* BFI: the lit pulse starts now (no-op unless Strobe is on) */
	{ extern void ayaneo_rewind_tables_tick(void); ayaneo_rewind_tables_tick(); }   /* rewind island page tables: compare + repair every frame */
	if (s_pace_expect) {
		if ((*(volatile unsigned int *)(0x14009000u + 0xF40u)) == s_pace_expect) g_dbg_latch_hit++;
		else g_dbg_latch_miss++;
	}
	g_dbg_pace_edge_us = (unsigned int)((int)(now - target)) / 13u;   /* how far past the deadline the edge was */
	g_dbg_pace_dt_us = (now - s_pace_edge) / 13u;
	s_pace_edge = now;
	g_dbg_pace_waits = n;
	{
		static unsigned int win0, cnt;
		cnt++;
		if (!win0) win0 = now;
		if (now - win0 >= 13000000u) { g_dbg_present_fps = cnt; cnt = 0; win0 = now; }
	}
}
#else
void ayaneo_frame_delay(void) {}
unsigned int ayaneo_pace_ticks_left(void) { return 0; }
#endif
/* NEON implementations (ayaneo_rot_neon.c, compiled with -mfpu=neon on the Konkr) */
void ayaneo_rot8888_neon(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
			 unsigned int *dst, unsigned dpitch, int ccw);
void ayaneo_rot_turn_band_neon(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
			       unsigned int *dst, unsigned dpitch, int ccw);
void ayaneo_rot8888_alpha_neon(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
			       unsigned int *dst, unsigned dpitch, int ccw);
void ayaneo_scale43_neon(const unsigned int *src, unsigned sw, unsigned sh, unsigned spitch,
			 unsigned int *dst, unsigned dw, unsigned dh, unsigned dpitch);
void ayaneo_canvas_present_neon(const unsigned int *src, unsigned sw, unsigned sh, unsigned spitch,
				unsigned int *fb, unsigned fbpitch, unsigned dw, unsigned dh, int ccw);
/* What `oem shot` should dump: the last resizer input (the game, turned, prescaled) while a
 * resizer session is up, otherwise the front frame buffer. All physical geometry. */
static unsigned int s_shot_pa, s_shot_w, s_shot_h, s_shot_pitch;

unsigned int ayaneo_screen_w(void) { return AYANEO_ROT ? CFG_DISPLAY_HEIGHT : CFG_DISPLAY_WIDTH; }
unsigned int ayaneo_screen_h(void) { return AYANEO_ROT ? CFG_DISPLAY_WIDTH : CFG_DISPLAY_HEIGHT; }

#if AYANEO_ROT
/* Map a logical rect onto the physical frame buffer. */
static void rot_rect(unsigned lx, unsigned ly, unsigned lw, unsigned lh,
		     unsigned *px, unsigned *py, unsigned *pw, unsigned *ph)
{
	unsigned PW = CFG_DISPLAY_WIDTH, PH = CFG_DISPLAY_HEIGHT;
	*pw = lh; *ph = lw;
	if (!g_rot_ccw) { *px = (ly + lh <= PW) ? PW - ly - lh : 0; *py = lx; }
	else            { *px = ly; *py = (lx + lw <= PH) ? PH - lx - lw : 0; }
}

/* Turn an RGB565 frame (sw x sh at spitch) into dst as (sh x sw) with pitch sh. */
static void rot565(const unsigned short *src, unsigned sw, unsigned sh, unsigned spitch,
		   unsigned short *dst)
{
	unsigned x, y;
	if (!g_rot_ccw) {
		/* dst[x][sh-1-y] = src[y][x]: walk the source rows, scatter down one dst column each */
		for (y = 0; y < sh; y++) {
			const unsigned short *sr = src + y * spitch;
			unsigned short *dc = dst + (sh - 1u - y);
			for (x = 0; x < sw; x++) dc[x * sh] = sr[x];
		}
	} else {
		/* dst[sw-1-x][y] = src[y][x] */
		for (y = 0; y < sh; y++) {
			const unsigned short *sr = src + y * spitch;
			unsigned short *dc = dst + (sw - 1u) * sh + y;
			for (x = 0; x < sw; x++) dc[-(int)(x * sh)] = sr[x];
		}
	}
}

/* Turn a 32-bit canvas (w x h at spitch words) into dst (h x w at dpitch words). Processes the
 * source in 8x8 tiles so both the reads and the scattered writes stay inside one cache line set;
 * ~1 ms for 960x640 on one core at LK clocks. */
static void rot8888(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
		    unsigned int *dst, unsigned dpitch)
{
	unsigned tx, ty, x, y;
	for (ty = 0; ty < h; ty += 8) {
		unsigned yh = (ty + 8u <= h) ? 8u : h - ty;
		for (tx = 0; tx < w; tx += 8) {
			unsigned xw = (tx + 8u <= w) ? 8u : w - tx;
			for (y = 0; y < yh; y++) {
				const unsigned int *sr = src + (ty + y) * spitch + tx;
				if (!g_rot_ccw) {
					unsigned int *dc = dst + (h - 1u - (ty + y));
					for (x = 0; x < xw; x++) dc[(tx + x) * dpitch] = sr[x];
				} else {
					unsigned int *dc = dst + (ty + y);
					for (x = 0; x < xw; x++) dc[(w - 1u - (tx + x)) * dpitch] = sr[x];
				}
			}
		}
	}
}

/* Exact 4:3 box downscale of an ARGB canvas: every 4 source columns become 3 destination
 * columns with weights (3,1)/4, (2,2)/4, (1,3)/4, the same vertically. Separable, integer, no
 * divides; 1280x853 -> 960x640 (the last partial row group clamps to the last source row). */
static void scale43_down(const unsigned int *src, unsigned sw, unsigned sh, unsigned spitch,
			 unsigned int *dst, unsigned dw, unsigned dh, unsigned dpitch)
{
	/* per-channel mix of two ARGB pixels: (a*wa + b*wb) / 4, wa + wb == 4 */
	#define MIX4(a, b, wa, wb) \
		(0xFF000000u | \
		 (((((a) >> 16) & 0xffu) * (wa) + (((b) >> 16) & 0xffu) * (wb)) >> 2) << 16 | \
		 (((((a) >>  8) & 0xffu) * (wa) + (((b) >>  8) & 0xffu) * (wb)) >> 2) <<  8 | \
		 (((( (a)      ) & 0xffu) * (wa) + (( (b)      ) & 0xffu) * (wb)) >> 2))
	unsigned x, y, g;
	for (y = 0; y < dh; y++) {
		/* destination row y blends source rows r0/r1 by the 4->3 pattern */
		unsigned grp = y / 3u, k = y % 3u, r0, r1, wa, wb;
		if (k == 0)      { r0 = grp * 4u;      r1 = r0 + 1u; wa = 3; wb = 1; }
		else if (k == 1) { r0 = grp * 4u + 1u; r1 = r0 + 1u; wa = 2; wb = 2; }
		else             { r0 = grp * 4u + 2u; r1 = r0 + 1u; wa = 1; wb = 3; }
		if (r0 >= sh) r0 = sh - 1u;
		if (r1 >= sh) r1 = sh - 1u;
		{
			const unsigned int *a = src + r0 * spitch, *b = src + r1 * spitch;
			unsigned int *o = dst + y * dpitch;
			for (x = 0, g = 0; x + 3u <= dw && g + 4u <= sw; x += 3u, g += 4u) {
				/* vertical mix of the four source columns of this group */
				unsigned int c0 = MIX4(a[g],      b[g],      wa, wb);
				unsigned int c1 = MIX4(a[g + 1u], b[g + 1u], wa, wb);
				unsigned int c2 = MIX4(a[g + 2u], b[g + 2u], wa, wb);
				unsigned int c3 = MIX4(a[g + 3u], b[g + 3u], wa, wb);
				o[x]      = MIX4(c0, c1, 3, 1);
				o[x + 1u] = MIX4(c1, c2, 2, 2);
				o[x + 2u] = MIX4(c2, c3, 1, 3);
			}
			for (; x < dw; x++) o[x] = 0xFF000000u;   /* only if dw is not a multiple of 3 */
		}
	}
	#undef MIX4
}
#endif

/* Largest integer scale of (sw x sh) that fits the logical screen. */
static unsigned rot_int_scale(unsigned sw, unsigned sh)
{
	unsigned k = ayaneo_screen_w() / sw, k2 = ayaneo_screen_h() / sh;
	if (k2 < k) k = k2;
	return k ? k : 1u;
}

/* ---- primitive drawing into a 32-bit BGRA back buffer (used by the menu) ---- */

/* Clip extent for the primitive drawers below. They clip to the panel by default, which is
 * right for the pairmini's panel sized buffers; the Konkr hands out logical / virtual canvases
 * that are wider than its portrait frame buffer, so whoever hands out such a buffer sets the
 * extent to that buffer's size (0,0 = back to the panel). */
static unsigned int s_clip_w, s_clip_h;
void ayaneo_draw_clip(unsigned int w, unsigned int h) { s_clip_w = w; s_clip_h = h; }
#define CLIP_W (s_clip_w ? s_clip_w : CFG_DISPLAY_WIDTH)
#define CLIP_H (s_clip_h ? s_clip_h : CFG_DISPLAY_HEIGHT)

/* filled rectangle, clipped to the buffer extent */
void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
		 int x, int y, int w, int h, unsigned int argb)
{
	unsigned int W = CLIP_W, H = CLIP_H;
	int iy, ix;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)W) w = (int)W - x;
	if (y + h > (int)H) h = (int)H - y;
	for (iy = 0; iy < h; iy++) {
		unsigned int *o = buf + (unsigned int)(y + iy) * pitch_w + x;
		for (ix = 0; ix < w; ix++)
			o[ix] = argb;
	}
}

/* alpha-blended filled rectangle (argb source over the existing buffer pixels),
 * alpha 0-255. Used for the translucent menu panel so the game shows through. */
void ayaneo_fill_blend(unsigned int *buf, unsigned int pitch_w,
		       int x, int y, int w, int h, unsigned int argb, int alpha)
{
	unsigned int Wd = CLIP_W, Hd = CLIP_H;
	/* 0-255 alpha mapped to 0-256 so the blend is a shift, not a divide (per-
	 * pixel divides were heavy enough to push the menu frame past one refresh
	 * and halve the emulation rate). Pre-scale the source term once. */
	unsigned int a = (unsigned int)alpha + ((unsigned int)alpha >> 7);	/* 0..256 */
	unsigned int ia = 256 - a;
	unsigned int sr = (((argb >> 16) & 0xff) * a);
	unsigned int sg = (((argb >> 8) & 0xff) * a);
	unsigned int sb = ((argb & 0xff) * a);
	int iy, ix;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)Wd) w = (int)Wd - x;
	if (y + h > (int)Hd) h = (int)Hd - y;
	for (iy = 0; iy < h; iy++) {
		unsigned int *o = buf + (unsigned int)(y + iy) * pitch_w + x;
		for (ix = 0; ix < w; ix++) {
			unsigned int d = o[ix];
			unsigned int r = (sr + ((d >> 16) & 0xff) * ia) >> 8;
			unsigned int g = (sg + ((d >> 8) & 0xff) * ia) >> 8;
			unsigned int b = (sb + (d & 0xff) * ia) >> 8;
			o[ix] = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
	}
}

/* Pico menu lists (every core's in-game menu): row pitch, panel height and the first row shown.
 * Rows are 38 px when the whole list fits. On a shorter screen they go down to 32 px, the least
 * that keeps the 2x font's ink (pixels 4..29 of its 32 px cell) inside the selection bar, and the
 * list scrolls to keep the selection in view instead of squeezing further (at 22 px the bar
 * covered only the top of the text and the rows ran into each other). Title band 60 px, footer
 * 34 px, 16 px margin, as before. Returns the number of rows shown. */
int ayaneo_pico_layout(int count, int H, int sel, int *top, int *rowH, int *panelH)
{
	int avail = H - 16 - 60 - 34, rh = 38, vis;
	if (count < 1) count = 1;
	if (count * rh > avail) { rh = avail / count; if (rh < 32) rh = 32; }
	vis = avail / rh;
	if (vis > count) vis = count;
	if (vis < 1) vis = 1;
	if (sel < *top) *top = sel;
	if (sel >= *top + vis) *top = sel - vis + 1;
	if (*top > count - vis) *top = count - vis;
	if (*top < 0) *top = 0;
	*rowH = rh;
	*panelH = 60 + vis * rh + 34;
	return vis;
}

/* the selection bar for a row whose 2x text is drawn at y: centred on the text's 32 px cell */
void ayaneo_pico_bar(unsigned int *buf, unsigned int pitch, int x, int w, int y, int rowH)
{
	ayaneo_fill(buf, pitch, x, y + 16 - rowH / 2, w, rowH, 0xFF5090F0u);
}

/* more rows above / below the ones shown: small triangles at the right of the title band and
 * of the footer */
void ayaneo_pico_more(unsigned int *buf, unsigned int pitch, int px, int py, int panelW, int panelH,
		      int up, int down)
{
	int r, cx = px + panelW - 28 - 8;
	for (r = 0; r < 8; r++) {
		if (up)   ayaneo_fill(buf, pitch, cx - r, py + 28 + r, 2 * r + 1, 1, 0xFFC8D0E0u);
		if (down) ayaneo_fill(buf, pitch, cx - r, py + panelH - 22 - r, 2 * r + 1, 1, 0xFFC8D0E0u);
	}
}

/* one glyph from the 8x16 font, integer-scaled */
static void ayaneo_glyph(unsigned int *buf, unsigned int pitch_w,
			 int x, int y, int scale, unsigned int argb, unsigned char c)
{
	const unsigned char *g = &mtk_vdo_fntdata[(unsigned int)c * MTK_VFH];
	int row, col, sy, sx;

	for (row = 0; row < MTK_VFH; row++) {
		unsigned char bits = g[row];
		for (col = 0; col < MTK_VFW; col++) {
			if (!(bits & (0x80 >> col)))
				continue;
			for (sy = 0; sy < scale; sy++) {
				unsigned int *o = buf +
					(unsigned int)(y + row * scale + sy) * pitch_w +
					(x + col * scale);
				for (sx = 0; sx < scale; sx++)
					o[sx] = argb;
			}
		}
	}
}

/* draw a NUL-terminated string; returns the x just past the last glyph */
int ayaneo_text(unsigned int *buf, unsigned int pitch_w,
		int x, int y, int scale, unsigned int argb, const char *s)
{
	for (; *s; s++) {
		if (*s != ' ')
			ayaneo_glyph(buf, pitch_w, x, y, scale, argb, (unsigned char)*s);
		x += MTK_VFW * scale;
	}
	return x;
}

/* ---- fast-forward / rewind speed HUD (top-right corner of the game rect) --------------------
 * The emu loop publishes the current action + speed each frame via ayaneo_hud_set(); the present
 * path draws a small translucent badge (a double-triangle icon + the speed, e.g. "4x" / "5.0x")
 * into the game rect. It is self-clearing: as soon as the loop stops publishing a mode the next
 * present's game blit overwrites the badge. Single-word volatile stores from the one emu thread,
 * read here in the one present flow - no locking. mode: 0=none, 1=fast-forward, 2=rewind.
 * speed_x10 = speed*10 (FF: frame multiplier*10 -> "Nx"; rewind: spd*10/256 -> "N.Mx"). */
volatile int g_hud_mode;
volatile int g_hud_speed_x10;
void ayaneo_hud_set(int mode, int speed_x10) { g_hud_mode = mode; g_hud_speed_x10 = speed_x10; }

/* Frame rate meter shared by every core: one call per emulated frame (fast forward and
 * benchmark frames included), the figure is refreshed every half second. Shown as a badge in
 * the top left of the game when the Show FPS setting is on or a core's benchmark runs. */
volatile int g_hud_fps;
static int s_hud_no_fps;   /* the rotated path draws the fps badge in its own box */
volatile int g_show_fps;
volatile int g_hud_bench;
volatile int g_hud_hide;   /* the BIOS intro and other non game screens */
void ayaneo_hud_hide(int on) { g_hud_hide = on ? 1 : 0; }
void ayaneo_fps_tick(void)
{
	static unsigned int t0, n;
	unsigned int now = gpt4_get_current_tick(), el;
	if (!t0) { t0 = now; n = 0; return; }
	n++;
	el = now - t0;
	if (el >= 6500000u) {                     /* half a second at 13 MHz */
		g_hud_fps = (int)((unsigned long long)n * 13000000ull / el);
		t0 = now; n = 0;
	}
}
void ayaneo_fps_reset(void) { g_hud_fps = 0; }
void ayaneo_hud_bench(int on) { g_hud_bench = on ? 1 : 0; }

/* Fast forward budget: the frame started at t0 (13 MHz ticks) and must end with the present
 * blit (blit_us) before the next panel edge. Another emulated frame fits when the time used so
 * far plus that frame (next_us), the final rendered frame (rend_us) and the blit still leave the
 * 1.2 ms edge margin. Every core asks this per frame and keeps running frames while it says yes:
 * the speed is whatever the CPU allows, the panel still gets one new picture per refresh. */
volatile unsigned int g_dbg_ff_margin_us = 1200, g_dbg_ff_misses, g_dbg_ff_frames;
int ayaneo_ff_fits(unsigned int t0, unsigned int blit_us, unsigned int next_us, unsigned int rend_us)
{
	extern unsigned int ayaneo_dsi_core_refresh_milli(void);
	static unsigned int s_last_t0;
	unsigned int rm = ayaneo_dsi_core_refresh_milli();
	unsigned int full = rm ? 1000000000u / rm : 16666u;
	unsigned int budget, used;
	/* Closed loop on the real outcome: t0 changes once per loop frame, so the gap between two
	 * t0 values is the achieved frame period. A gap over 1.5 refreshes means the batch ran
	 * past the panel edge and the present slipped a whole refresh (60 became 30): widen the
	 * margin at once; relax it slowly while frames land. The estimates feeding this (frame
	 * costs averaged over a few frames) cannot see a dynarec translation burst, the loop can. */
	if (t0 != s_last_t0) {
		unsigned int gap = (t0 - s_last_t0) / 13u;
		if (s_last_t0 && gap < 10u * full) {
			g_dbg_ff_frames++;
			if (gap > full + full / 2u) { g_dbg_ff_misses++; if (g_dbg_ff_margin_us < 9000u) g_dbg_ff_margin_us += 600u; }
			else if (g_dbg_ff_margin_us > 1200u) g_dbg_ff_margin_us -= 25u;
		}
		s_last_t0 = t0;
	}
	budget = full > g_dbg_ff_margin_us ? full - g_dbg_ff_margin_us : 0u;
	used = (gpt4_get_current_tick() - t0) / 13u;
	{	/* with the next edge known, budget to it: the loop may have started later than the last
		 * edge (frame delay), and the edge is what the present has to make */
		extern unsigned int ayaneo_pace_ticks_left(void);
		unsigned int left = ayaneo_pace_ticks_left() / 13u;
		if (left)
			return blit_us + next_us + rend_us + g_dbg_ff_margin_us < left;
	}
	return used + blit_us + next_us + rend_us < budget;
}
#define AYANEO_FF_MAX_FRAMES 120   /* a guard against a zero cost estimate, not a speed cap */
int ayaneo_hud_fps_visible(void) { return !g_hud_hide && (g_show_fps || g_hud_bench); }
/* the FPS badge text and its box size (the same layout ayaneo_hud_draw_fps draws) */
static int ayaneo_hud_fps_text(char *s)
{
	int n = 0, v = g_hud_fps, t[8], k = 0;
	if (v < 0) v = 0; if (v > 9999) v = 9999;
	if (!v) s[n++] = '0'; else { while (v) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) s[n++] = t[--k]; }
	s[n++] = ' '; s[n++] = 'F'; s[n++] = 'P'; s[n++] = 'S'; s[n] = 0;
	return n;
}
static void __attribute__((unused)) ayaneo_hud_fps_size(int *w, int *h)
{
	char s[12];
	int n = ayaneo_hud_fps_text(s);
	*w = 10 + n * MTK_VFW * 2 + 10;
	*h = MTK_VFH * 2 + 12;
}
/* the FPS badge, top left of the game rect */
static void ayaneo_hud_draw_fps(unsigned int *buf, unsigned int pitch_w, int gx, int gy)
{
	char s[12]; int n;
	const int scale = 2, pad = 10;
	int th = MTK_VFH * scale, bh = th + 12, bw;
	if (!ayaneo_hud_fps_visible()) return;
	n = ayaneo_hud_fps_text(s);
	bw = pad + n * MTK_VFW * scale + pad;
	ayaneo_fill_blend(buf, pitch_w, gx + 12, gy + 12, bw, bh, 0xFF0A0A0Fu, 190);
	ayaneo_text(buf, pitch_w, gx + 12 + pad, gy + 12 + (bh - th) / 2, scale, 0xFF3CFF78u, s);
}

/* one filled triangle in a size x size box, pointing right (right=1) or left (right=0) */
static void ayaneo_hud_tri(unsigned int *buf, unsigned int pitch_w, int x0, int y0, int size,
			   int right, unsigned int argb)
{
	int r;
	for (r = 0; r < size; r++) {
		int d = (size - 1) - 2 * r; if (d < 0) d = -d;   /* |..| -> span 1..size..1 */
		int w = size - d; if (w < 1) w = 1;
		int xs = right ? x0 : (x0 + size - w);
		ayaneo_fill(buf, pitch_w, xs, y0 + r, w, 1, argb);   /* ayaneo_fill clips to the panel */
	}
}

/* Draw the FF/RW badge into the top-right of the game rect [gx,gy,gw,gh] in `buf` (pitch_w px). */
static void ayaneo_hud_draw(unsigned int *buf, unsigned int pitch_w, int gx, int gy, int gw, int gh)
{
	int mode = g_hud_mode, sx10 = g_hud_speed_x10;
	unsigned int col;
	char s[8]; int n = 0, whole, frac;
	const int scale = 2, icon = 22, gap = 8, pad = 10;
	int th = MTK_VFH * scale, bh = th + 12, bw, bx, by, iy, tx;
	(void)gh;
	if (!s_hud_no_fps) ayaneo_hud_draw_fps(buf, pitch_w, gx, gy);
	if (mode != 1 && mode != 2) return;
	col = (mode == 1) ? 0xFF3CFF78u : 0xFF46C8FFu;       /* FF green / RW cyan */
	whole = sx10 / 10; frac = sx10 % 10;
	if (whole < 0) whole = 0; if (frac < 0) frac = 0;
	if (whole >= 10) s[n++] = (char)('0' + (whole / 10) % 10);
	s[n++] = (char)('0' + whole % 10);
	if (mode == 2) { s[n++] = '.'; s[n++] = (char)('0' + frac); }   /* rewind shows one decimal */
	s[n++] = 'x'; s[n] = 0;
	bw = pad + icon * 2 + gap + n * MTK_VFW * scale + pad;
	bx = gx + gw - bw - 12;
	by = gy + 12;
	if (bx < gx) bx = gx;
	ayaneo_fill_blend(buf, pitch_w, bx, by, bw, bh, 0xFF0A0A0Fu, 190);   /* translucent dark pill */
	iy = by + (bh - icon) / 2;
	ayaneo_hud_tri(buf, pitch_w, bx + pad,        iy, icon, mode == 1, col);
	ayaneo_hud_tri(buf, pitch_w, bx + pad + icon, iy, icon, mode == 1, col);
	tx = bx + pad + icon * 2 + gap;
	ayaneo_text(buf, pitch_w, tx, by + (bh - th) / 2, scale, 0xFFFFFFFFu, s);
}

/* Get the current back buffer (the one not being scanned out) to draw a full
 * frame into. The menu uses this + ayaneo_canvas_present() to render itself. */
unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H)
{
#if AYANEO_ROT
	/* The virtual 1280x853 canvas; ayaneo_canvas_present scales + turns it into the back buffer. */
	*pitch_w = ROT_CANVAS_W;
	*W = ROT_CANVAS_W;
	*H = ROT_CANVAS_H;
	ayaneo_draw_clip(ROT_CANVAS_W, ROT_CANVAS_H);
	return (unsigned int *)ROT_CANVAS_PA;
#else
	*pitch_w = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT);
	*W = CFG_DISPLAY_WIDTH;
	*H = CFG_DISPLAY_HEIGHT;
	return (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
#endif
}

/* Get the currently DISPLAYED (front) buffer - the one being scanned out, i.e.
 * whatever was last presented. Used by the fastboot debug channel to screenshot
 * the live panel content while the menu/game runs. present() flips s_fb_flip
 * AFTER latching, so the displayed buffer is the opposite of the current back. */
const unsigned int *ayaneo_canvas_front(unsigned int *pitch_w, unsigned int *W, unsigned int *H)
{
#if AYANEO_ROT
	/* The virtual canvas is what was last presented (it is scaled + turned into the fb, never
	 * drawn into directly), so it doubles as the readable front image. */
	return ayaneo_canvas_back(pitch_w, W, H);
#else
	*pitch_w = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT);
	*W = CFG_DISPLAY_WIDTH;
	*H = CFG_DISPLAY_HEIGHT;
	return (const unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? 0 : fb_size));
#endif
}

/* Source for the fastboot `oem shot` dump (see s_shot_*). Returns 1 when a resizer session is
 * live and the game buffer is returned, 0 for the front frame buffer. */
static int s_rsz_setup;   /* tentative; the RSZ path defines it below */
/* The in-game overlay as painted (logical landscape 960x640 on the Konkr), for oem shot:N:o. */
int ayaneo_shot_overlay(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px)
{
#if AYANEO_ROT
	*pa = ROT_OVERLAY_PA; *w = ayaneo_screen_w(); *h = ayaneo_screen_h(); *pitch_px = ayaneo_screen_w();
	return 1;
#else
	(void)pa; (void)w; (void)h; (void)pitch_px;
	return 0;
#endif
}

int ayaneo_shot_source(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px)
{
	if (s_rsz_setup && s_shot_pa) {
		*pa = s_shot_pa; *w = s_shot_w; *h = s_shot_h; *pitch_px = s_shot_pitch;
		return 1;
	}
	*pa = (unsigned int)fb_addr_pa + (s_fb_flip ? 0 : fb_size);
	*w = CFG_DISPLAY_WIDTH; *h = CFG_DISPLAY_HEIGHT; *pitch_px = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT);
	return 0;
}

/* flush the whole back buffer and present it, then flip */
extern int priamry_display_wait_for_vsync(void);   /* primary_display.c (name has the typo) */
void ayaneo_snes_rsz_restore(void);   /* defined with the SNES RSZ path below */
/* Shared hardware-RSZ present + its state, used by the GBA/GBC show_frame paths above their
 * definitions further down (SNES/GBA/GBC all route Fit/Stretch through this). */
void ayaneo_rsz_present(const unsigned short *pix, unsigned int sw, unsigned int sh, unsigned int spitch_px,
			unsigned int dw, unsigned int dh, unsigned int xoff, unsigned int yoff,
			int filt, int wait_vsync, const unsigned short *cc_lut);
static int s_rsz_setup;               /* 1 while the RSZ path is configured (tentative def; also below) */
extern volatile unsigned g_rsz_show_us;

void ayaneo_canvas_present(void)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);

	/* Self-heal: if a SNES RSZ session left the pipe non-1:1, restore the full-panel path before
	 * presenting a panel-sized canvas frame (e.g. the exit reverse-punch). Trigger on EITHER the
	 * resizer still enabled OR the OVL0_2L ROI narrower than the panel (a padded-ROI residue the
	 * enable-bit check alone would miss after a clean disable). */
	if ((*(volatile unsigned int *)0x1401A000u & 1u) ||
	    (*(volatile unsigned int *)0x14009020u & 0xffffu) != W)
		ayaneo_snes_rsz_restore();

#if AYANEO_ROT
	{
		extern unsigned int gpt4_get_current_tick(void);
		unsigned int t0 = gpt4_get_current_tick();
		/* a canvas the size of the logical screen is just turned; a larger (4:3 wider) one goes
		 * through the fused box downscale + quarter turn */
		if (ROT_CANVAS_W == ayaneo_screen_w() && ROT_CANVAS_H == ayaneo_screen_h())
			ayaneo_rot_turn_band_neon((const unsigned int *)ROT_CANVAS_PA, ROT_CANVAS_W, ROT_CANVAS_H, ROT_CANVAS_W,
						  (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0)), pitch_w, g_rot_ccw);
		else
			ayaneo_canvas_present_neon((const unsigned int *)ROT_CANVAS_PA, ROT_CANVAS_W, ROT_CANVAS_H, ROT_CANVAS_W,
						   (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0)), pitch_w,
						   ayaneo_screen_w(), ayaneo_screen_h(), g_rot_ccw);
		g_dbg_present_us[0] = (gpt4_get_current_tick() - t0) / 13u;
		g_dbg_present_us[1] = 0;
	}
#endif
	arch_clean_cache_range((unsigned int)((unsigned char *)fb_addr +
			       (s_fb_flip ? fb_size : 0)), fb_size);
#if AYANEO_ROT
	{
		extern int ayaneo_present_skip_framedone;
		int save = ayaneo_present_skip_framedone;
		rot_pace();                            /* deadline + phase: returns right after a frame end */
		ayaneo_present_skip_framedone = 1;     /* no frame-done block inside: we just aligned */
		ayaneo_present(dpa, W, H, pitch_w);
		ayaneo_present_skip_framedone = save;
		s_pace_expect = dpa;                   /* the next frame end must show this buffer */
		s_fb_flip ^= 1;
		return;
	}
#endif
	ayaneo_present(dpa, W, H, pitch_w);
	/* Video mode: the OVL latches the new buffer address at the NEXT vsync, but
	 * primary_display_trigger returns immediately. Without waiting, the loop starts
	 * redrawing the OTHER buffer while THIS one is still being scanned out -> tearing
	 * + partial-black on every present = the "flickering like crazy" the user saw on
	 * static screens (suspend list) after the present gate was removed. Block for one
	 * vsync so the swap is live before we hand the old buffer back to the renderer.
	 * This is exactly what the flicker-free b369be3 build did; dropping it was the
	 * regression. It also paces the loop cleanly to the panel. */
	priamry_display_wait_for_vsync();
	{
		extern volatile unsigned int g_vsync_div;
		unsigned int k;
		for (k = 1u; k < g_vsync_div; k++) priamry_display_wait_for_vsync();
	}
	s_fb_flip ^= 1;
}

/*
 * Draw (or clear) the OSD slider in the bottom letterbox border of the current
 * back buffer. The border band is only ever touched by the OSD, so when the OSD
 * expires we paint it black for a couple of frames to clear both buffers. Colours
 * distinguish the two controls: volume = cyan, brightness = amber.
 */
static void ayaneo_draw_osd(unsigned int *dst, unsigned int pitch_w,
			    unsigned int W, unsigned int H)
{
	static int clear_left;
	unsigned int dh = GBC_SRC_H * GBC_SCALE, yoff = (H - dh) / 2;
	int active = s_osd_kind &&
		     (int)(s_osd_until_ms - (unsigned int)current_time()) > 0;
	unsigned int barW, barH, bx, by, fillW, fill, border, x, y;

	if (active)
		clear_left = 2;		/* keep clearing after it expires */
	else if (clear_left > 0)
		clear_left--;
	else
		return;

	barW = (W * 3) / 5;		/* ~60% of the panel width */
	barH = 22;
	bx = (W - barW) / 2;
	by = yoff + dh + (yoff > barH + 8 ? (yoff - barH) / 2 : 4);
	if (by + barH >= H)		/* safety: keep inside the panel */
		by = H - barH - 1;

	fill   = active ? (s_osd_kind == 1 ? 0xFF20D0FFu : 0xFFFFC020u) : 0xFF000000u;
	border = active ? 0xFFFFFFFFu : 0xFF000000u;
	fillW  = active ? ((barW - 4) * (unsigned int)s_osd_pct) / 100u : 0;

	for (y = 0; y < barH; y++) {
		unsigned int *o = dst + (by + y) * pitch_w + bx;
		int edge_row = (y < 2 || y >= barH - 2);
		for (x = 0; x < barW; x++) {
			unsigned int c;
			if (!active)
				c = 0xFF000000u;		/* clearing */
			else if (edge_row || x < 2 || x >= barW - 2)
				c = border;			/* 2px frame */
			else
				c = (x - 2 < fillW) ? fill : 0xFF202020u;
			o[x] = c;
		}
	}
	arch_clean_cache_range((unsigned int)(dst + by * pitch_w), barH * pitch_w * 4);
}

extern int ayaneo_get_lcd_filter(void);		/* ayaneo_audio.c */
extern int ayaneo_get_lcd_filter_core(int c);	/* ayaneo_audio.c: 0=SNES 1=GBA 2=GBC */
extern int ayaneo_get_color_correct(void);	/* ayaneo_audio.c */
extern int gbc_menu_is_open(void);		/* gbc_driver.c */
extern void gbc_menu_draw_overlay(unsigned int *buf, unsigned int pitch,
				  unsigned int W, unsigned int H);
extern int gbc_benchmark_on(void);		/* gbc_driver.c */
extern int gbc_get_fps(void);
extern int ayaneo_wait_frame_done(void);	/* primary_display.c */

/* Clear both scan-out buffers, disable the boot-menu layer so only our FB_LAYER
 * shows, and bring the backlight up at the persisted level. Idempotent; shared
 * by the emulator display and the offline-charging screen. */
/* Fill both scan-out buffers with 'argb', disable the boot-menu layer so only our
 * FB_LAYER shows, and bring the backlight up. The fill colour matters for the
 * handover: in DSI video mode one of these buffers is being scanned RIGHT NOW, so
 * memset-ing it is visible on the panel immediately. Black gives a black flash
 * between the BIOS intro and the menu; white gives the intended whiteout the menu
 * then fades in from (a seamless handover). */
static void ayaneo_display_prepare_fill(unsigned int argb)
{
	disp_input_config din;
	unsigned int i, n = fb_size / 4;
	unsigned int *p0 = (unsigned int *)fb_addr;
	unsigned int *p1 = (unsigned int *)((unsigned char *)fb_addr + fb_size);

	for (i = 0; i < n; i++) { p0[i] = argb; p1[i] = argb; }
	arch_clean_cache_range((unsigned int)fb_addr, fb_size);
	arch_clean_cache_range((unsigned int)fb_addr + fb_size, fb_size);
	memset(&din, 0, sizeof(din));
	din.layer = BOOT_MENU_LAYER;
	din.layer_en = 0;
	primary_display_config_input(&din);
	mt65xx_backlight_on();
	ayaneo_apply_persisted_brightness();
}

/* Idempotent; shared by the emulator display and the offline-charging screen. */
void ayaneo_display_prepare(void)
{
	ayaneo_display_prepare_fill(0x00000000u);
}

/* Whiteout variant for the BIOS-intro -> menu handover: paints the live buffer
 * white (not black) so there is no black flash before the menu fades in. */
void ayaneo_display_prepare_white(void)
{
	ayaneo_display_prepare_fill(0xFFFFFFFFu);
}

volatile unsigned int g_dbg_blit_us;	/* show_frame blit+composite us (excl vsync wait) */
void ayaneo_gbc_show_frame(const unsigned short *pix)
{
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int t_blit0 = gpt4_get_current_tick();
	static int inited = 0;
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int dw = GBC_SRC_W * GBC_SCALE, dh = GBC_SRC_H * GBC_SCALE;
	unsigned int xoff = (W - dw) / 2, yoff = (H - dh) / 2;
	unsigned int *dst;
	unsigned int dpa;
	unsigned int sx, sy, ix, iy;

	if (!inited) {
		ayaneo_display_prepare();
		inited = 1;
	}

#if AYANEO_ROT
	/* Turned panel: every aspect goes through the resizer path, which rotates the native frame
	 * and places it on the logical screen. Pixel is the exact integer fit (4x = 960x640 here). */
	{
		extern volatile int g_gba_aspect;
		extern int ayaneo_get_color_correct(void);
		extern const unsigned short gba_cc_lut444[];
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), dwr, dhr;
		if (g_gba_aspect == 2) { dwr = LW; dhr = LH; }
		else if (g_gba_aspect == 1) {
			if (LW * GBC_SRC_H <= LH * GBC_SRC_W) { dwr = LW; dhr = LW * GBC_SRC_H / GBC_SRC_W; }
			else { dhr = LH; dwr = LH * GBC_SRC_W / GBC_SRC_H; }
		} else { unsigned k = rot_int_scale(GBC_SRC_W, GBC_SRC_H); dwr = GBC_SRC_W * k; dhr = GBC_SRC_H * k; }
		ayaneo_rsz_present(pix, GBC_SRC_W, GBC_SRC_H, GBC_SRC_W, dwr, dhr,
				   (LW - dwr) / 2u, (LH - dhr) / 2u, ayaneo_get_lcd_filter_core(1), 0,
				   ayaneo_get_color_correct() ? gba_cc_lut444 : 0);
		g_dbg_blit_us = g_rsz_show_us;
		return;
	}
#endif

	/* Aspect modes (GBA 240x160): Fit / Stretch scale via the hardware RSZ; Pixel (0) uses the
	 * sharp integer CPU blit below. RSZ stays on while the live Pico menu overlay is up (it
	 * composites on top as OVL0 L0; g_overlay_active single-buffers the RSZ scratch so it never
	 * collides with the overlay buffer). Selecting Pixel - or exiting the session (canvas_present
	 * self-heal) - restores the 1:1 path via ayaneo_snes_rsz_restore. */
	{
		extern volatile int g_gba_aspect;
		extern int ayaneo_get_color_correct(void);
		extern const unsigned short gba_cc_lut444[];
		if (g_gba_aspect != 0) {
			unsigned int dwr, dhr;
			if (g_gba_aspect == 2) { dwr = W; dhr = H; }   /* Stretch: full panel */
			else if (W * GBC_SRC_H <= H * GBC_SRC_W) { dwr = W; dhr = W * GBC_SRC_H / GBC_SRC_W; }
			else { dhr = H; dwr = H * GBC_SRC_W / GBC_SRC_H; }   /* Fit: native aspect, max fill */
			ayaneo_rsz_present(pix, GBC_SRC_W, GBC_SRC_H, GBC_SRC_W, dwr, dhr,
					   (W - dwr) / 2u, (H - dhr) / 2u, ayaneo_get_lcd_filter_core(1), 0,
					   ayaneo_get_color_correct() ? gba_cc_lut444 : 0);
			g_dbg_blit_us = g_rsz_show_us;   /* keep GBA run-ahead sizing (preempt_adapt) fed */
			return;
		}
		if (s_rsz_setup) ayaneo_snes_rsz_restore();   /* menu open / Pixel: back to 1:1 (once) */
	}

	dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);

	{
		/* LCD filter: 1 scanlines (dim last row of each 6x block), 2 grid
		 * (dim last row + col), 3 both/heavier. Cheap per-subpixel darkening. */
		int filt = ayaneo_get_lcd_filter_core(1);   /* GBA */
		/* gpSP color correction: map each source pixel through the GBA LCD gamma
		 * LUT (indexed by RGB555, red high) so colors match real hardware instead
		 * of the oversaturated raw output. Applied once per 240x160 source pixel
		 * (before the 5x upscale), gated by the persisted setting. */
		int cc = ayaneo_get_color_correct();
		extern const unsigned short gba_cc_lut444[];
		if (!filt) {
			/* Fast path (no LCD filter, the common case): render ONE destination row per
			 * source row (scalar unpack + GBC_SCALE-wide horizontal replicate), then copy it to
			 * the remaining GBC_SCALE-1 rows with memcpy (wide vectorised stores). The old
			 * per-pixel GBC_SCALE x GBC_SCALE scalar block wrote every scaled row by hand
			 * (~1M u32 stores/frame, measured ~7.6 ms at 600 MHz); this cuts the vertical scale
			 * to ~1 scalar row + N cheap copies, matching the Genesis/SNES fast path. */
			for (sy = 0; sy < GBC_SRC_H; sy++) {
				const unsigned short *srow = pix + sy * GBC_SRC_W;
				unsigned int *o0 = dst + (yoff + sy * GBC_SCALE) * pitch_w + xoff;
				unsigned int col = 0;
				for (sx = 0; sx < GBC_SRC_W; sx++) {
					unsigned int v = srow[sx];
					if (cc) v = gba_cc_lut444[(((v >> 12) & 0xF) << 8) |
								  (((v >> 7) & 0xF) << 4) |
								  ((v >> 1) & 0xF)];
					unsigned int r = ((v >> 11) & 0x1f) << 3;
					unsigned int g = ((v >> 5) & 0x3f) << 2;
					unsigned int b = (v & 0x1f) << 3;
					unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
					for (ix = 0; ix < GBC_SCALE; ix++) o0[col++] = px;
				}
				for (iy = 1; iy < GBC_SCALE; iy++)
					memcpy(o0 + iy * pitch_w, o0, (size_t)dw * 4u);
			}
		} else
		for (sy = 0; sy < GBC_SRC_H; sy++) {
			const unsigned short *srow = pix + sy * GBC_SRC_W;
			for (sx = 0; sx < GBC_SRC_W; sx++) {
				unsigned int v = srow[sx];
				if (cc) v = gba_cc_lut444[(((v >> 12) & 0xF) << 8) |
							  (((v >> 7) & 0xF) << 4) |
							  ((v >> 1) & 0xF)];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
				unsigned int dk = 0xFF000000u |
					((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
				for (iy = 0; iy < GBC_SCALE; iy++) {
					unsigned int *o = dst +
						(yoff + sy * GBC_SCALE + iy) * pitch_w +
						(xoff + sx * GBC_SCALE);
					int lastrow = (iy == GBC_SCALE - 1);
					for (ix = 0; ix < GBC_SCALE; ix++) {
						unsigned int c = px;
						int lastcol = (ix == GBC_SCALE - 1);
						if (filt == 1 && lastrow) c = dk;
						else if (filt == 2 && (lastrow || lastcol)) c = dk;
						else if (filt == 3 && (lastrow || lastcol)) c = dk;
						o[ix] = c;
					}
				}
			}
		}
	}
	if (!gbc_menu_is_open())			/* menu is now a hardware overlay (OVL0 L0); only the OSD paints in-frame */
		ayaneo_draw_osd(dst, pitch_w, W, H);	/* volume/brightness slider */
	ayaneo_hud_draw(dst, pitch_w, (int)xoff, (int)yoff, (int)dw, (int)dh);   /* FF/RW speed badge */
	arch_clean_cache_range((unsigned int)(dst + yoff * pitch_w), dh * pitch_w * 4);
	g_dbg_blit_us = (gpt4_get_current_tick() - t_blit0) / 13u;	/* work before the vsync wait */
	ayaneo_present(dpa, W, H, pitch_w);
	s_fb_flip ^= 1;
}

/* Real GB/GBC frame (gambatte): 160x144 native, integer 6x -> 960x864 centred on the
 * 1280x960 panel. A DEDICATED path (not ayaneo_gbc_show_frame, which in the AYANEO_GBA
 * build is compiled for the GBA game's 240x160x5 geometry and would stretch a 160x144
 * frame to GBA aspect). No GBA colour-correction LUT (gambatte already outputs final
 * RGB565); honours the LCD filter + volume OSD. Used by emu/gbc/gbc_sd_run.c. */
void ayaneo_gb_show_frame(const unsigned short *pix)
{
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int t0 = gpt4_get_current_tick();
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	const unsigned int SW = 160, SH = 144, SC = 6;
	unsigned int dw = SW * SC, dh = SH * SC;              /* 960 x 864 */
	unsigned int xoff = (W - dw) / 2, yoff = (H - dh) / 2;
	unsigned int *dst;
	unsigned int dpa;
	int filt = ayaneo_get_lcd_filter_core(2);   /* GBC */
	unsigned int sx, sy, ix, iy;

#if AYANEO_ROT
	{
		extern volatile int g_gbc_aspect;
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), dwr, dhr;
		if (g_gbc_aspect == 2) { dwr = LW; dhr = LH; }
		else if (g_gbc_aspect == 1) {
			if (LW * SH <= LH * SW) { dwr = LW; dhr = LW * SH / SW; }
			else { dhr = LH; dwr = LH * SW / SH; }
		} else { unsigned k = rot_int_scale(SW, SH); dwr = SW * k; dhr = SH * k; }
		ayaneo_rsz_present(pix, SW, SH, SW, dwr, dhr, (LW - dwr) / 2u, (LH - dhr) / 2u, filt, 0, 0);
		g_dbg_blit_us = g_rsz_show_us;
		return;
	}
#endif
	/* Aspect modes (GB/GBC 160x144): Fit / Stretch via hardware RSZ; Pixel (0) uses the sharp
	 * integer CPU blit below. No colour-correction LUT (gambatte already outputs final RGB565).
	 * As on GBA, RSZ stays on under the live overlay menu; Pixel/exit restores the 1:1 path. */
	{
		extern volatile int g_gbc_aspect;
		if (g_gbc_aspect != 0) {
			unsigned int dwr, dhr;
			if (g_gbc_aspect == 2) { dwr = W; dhr = H; }   /* Stretch: full panel */
			else if (W * SH <= H * SW) { dwr = W; dhr = W * SH / SW; }
			else { dhr = H; dwr = H * SW / SH; }           /* Fit: native aspect, max fill */
			ayaneo_rsz_present(pix, SW, SH, SW, dwr, dhr, (W - dwr) / 2u, (H - dhr) / 2u, filt, 0, 0);
			g_dbg_blit_us = g_rsz_show_us;
			return;
		}
		if (s_rsz_setup) ayaneo_snes_rsz_restore();   /* menu open / Pixel: back to 1:1 (once) */
	}

	dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);

	if (!filt) {
		/* Fast path: one scalar dest row per source row, then memcpy-replicate to the other
		 * SC-1 rows (mirrors the Genesis/SNES/GBA fast path) - cuts the vertical 6x scale from
		 * 6 scalar rows to 1 scalar row + 5 wide copies. */
		for (sy = 0; sy < SH; sy++) {
			const unsigned short *srow = pix + sy * SW;
			unsigned int *o0 = dst + (yoff + sy * SC) * pitch_w + xoff;
			unsigned int col = 0;
			for (sx = 0; sx < SW; sx++) {
				unsigned int v = srow[sx];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
				for (ix = 0; ix < SC; ix++) o0[col++] = px;
			}
			for (iy = 1; iy < SC; iy++)
				memcpy(o0 + iy * pitch_w, o0, (size_t)dw * 4u);
		}
	} else
	for (sy = 0; sy < SH; sy++) {
		const unsigned short *srow = pix + sy * SW;
		for (sx = 0; sx < SW; sx++) {
			unsigned int v = srow[sx];
			unsigned int r = ((v >> 11) & 0x1f) << 3;
			unsigned int g = ((v >> 5) & 0x3f) << 2;
			unsigned int b = (v & 0x1f) << 3;
			unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
			unsigned int dk = 0xFF000000u | ((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
			for (iy = 0; iy < SC; iy++) {
				unsigned int *o = dst + (yoff + sy * SC + iy) * pitch_w + (xoff + sx * SC);
				int lastrow = (iy == (int)SC - 1);
				for (ix = 0; ix < SC; ix++) {
					unsigned int c = px; int lastcol = (ix == (int)SC - 1);
					if (filt == 1 && lastrow) c = dk;
					else if (filt >= 2 && (lastrow || lastcol)) c = dk;
					o[ix] = c;
				}
			}
		}
	}
	/* The Pico menu is now a hardware overlay (OVL0 L0); only the OSD paints in-frame. */
	{
		extern int gbc_menu_open(void);
		if (!gbc_menu_open())
			ayaneo_draw_osd(dst, pitch_w, W, H);         /* volume/brightness slider */
	}
	ayaneo_hud_draw(dst, pitch_w, (int)xoff, (int)yoff, (int)dw, (int)dh);   /* FF/RW speed badge */
	arch_clean_cache_range((unsigned int)(dst + yoff * pitch_w), dh * pitch_w * 4);
	g_dbg_blit_us = (gpt4_get_current_tick() - t0) / 13u;
	ayaneo_present(dpa, W, H, pitch_w);
	s_fb_flip ^= 1;
}

/* BIOS-logo intro only: scale the 240x160 frame 6x = 1440x960 to FILL the panel
 * HEIGHT, centred and cropped to the 1280 width (nearest neighbour). Efficient -
 * the visible source-column range is computed once so the inner loop has no
 * per-pixel clipping (that slowed the game). No LCD filter. */
void ayaneo_gba_show_intro_frame(const unsigned short *pix)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	const int S = 6, SRC_W = 240, SRC_H = 160;
	int xoff = ((int)W - SRC_W * S) / 2;		/* -80: overhang cropped */
	int sx0 = (-xoff + S - 1) / S;			/* first fully-visible src col */
	int sx1 = ((int)W - xoff) / S;			/* one past last fully-visible */
	unsigned int *dst;
	unsigned int dpa;
	int sx, sy, ix, iy;

#if AYANEO_ROT
	/* Fill the logical height like the 6x path does on the 4:3 panel; here that is the exact
	 * 4x = 960x640, nothing to crop. */
	{
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), k = rot_int_scale(SRC_W, SRC_H);
		(void)S; (void)xoff; (void)sx0; (void)sx1; (void)pitch_w; (void)dst; (void)dpa;
		(void)sx; (void)sy; (void)ix; (void)iy;
		ayaneo_rsz_present(pix, SRC_W, SRC_H, SRC_W, SRC_W * k, SRC_H * k,
				   (LW - SRC_W * k) / 2u, (LH - SRC_H * k) / 2u, 0, 0, 0);
		return;
	}
#endif
	if (sx0 < 0) sx0 = 0;
	if (sx1 > SRC_W) sx1 = SRC_W;
	dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);

	for (sy = 0; sy < SRC_H; sy++) {
		const unsigned short *srow = pix + sy * SRC_W;
		int dy0 = sy * S;
		for (sx = sx0; sx < sx1; sx++) {
			unsigned int v = srow[sx];
			unsigned int r = ((v >> 11) & 0x1f) << 3;
			unsigned int g = ((v >> 5) & 0x3f) << 2;
			unsigned int b = (v & 0x1f) << 3;
			unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
			int dx = xoff + sx * S;
			for (iy = 0; iy < S; iy++) {
				unsigned int *o = dst + (unsigned int)(dy0 + iy) * pitch_w + dx;
				for (ix = 0; ix < S; ix++)
					o[ix] = px;
			}
		}
	}
	arch_clean_cache_range((unsigned int)dst, H * pitch_w * 4);
	ayaneo_present(dpa, W, H, pitch_w);
	s_fb_flip ^= 1;
}

/* ---- EXPERIMENTAL hardware-resizer path (oem snes-rsz toggle, default OFF) ----------------
 * FB_LAYER (0) sits on OVL0_2L, which feeds RSZ0 in the DDP path (OVL0_2L -> RSZ0 -> OVL0). So
 * instead of the CPU upscaling 256x224 -> 1024x896 (the ~1840us blit), hand the native 256x224
 * frame to RSZ0 and let the hardware scale it. Iteration 1: program RSZ0 directly (the LK
 * ddp_rsz.c is a clock-only stub) and present the small buffer. Default OFF; when off, the
 * proven CPU-blit path below runs unchanged. Toggle with `fastboot oem snes-rsz:1`. */
volatile int g_snes_rsz = 1;                      /* 1 = hardware RSZ path (default ON for SNES) */
/* GBA/GBC display aspect: 0=Pixel (integer CPU blit, sharp, RSZ off), 1=Fit (native aspect scaled
 * to fill the panel, RSZ), 2=Stretch (full panel, RSZ). Set by each core's menu, read by its
 * show_frame path. Default Pixel = today's behaviour, so nothing changes until the user opts in. */
volatile int g_gba_aspect;
volatile int g_gbc_aspect;
enum { AR_PIXEL = 0, AR_FIT = 1, AR_STRETCH = 2 };
volatile unsigned g_snes_rsz_dbg;                 /* readback of RSZ enable/in/out for oem diag */
volatile unsigned g_rszdbg[12];                   /* live register readbacks captured at RSZ setup */
#define SNES_RSZ_BUF   0x54000000u                /* native-res ARGB scratch (safe DRAM window) */
/* MDP_RSZ0 base: mt_reg_base.h (the authoritative APB map) puts mdp_rsz at 0x14003000 -
 * 0x14002000 is MDP_RDMA1. project.h mislabels 0x14002000 as MDP_RSZ0, so programming there
 * was poking an idle RDMA and the real RSZ0 stayed in passthrough (256-in-1280 -> 5x repeat).
 * DISP_RSZ0 base is 0x1401A000 per the mt6785 vendor kernel dts ("mediatek,disp_rsz0") - NOT
 * MDP_RSZ0 at 0x14003000 (a separate MDP-domain block that accepts config but is not in the
 * display path; programming it configures cleanly and scales nothing).
 * Register layout is DISP_RSZ (CONTROL_1 0x004, CONTROL_2 0x008, INPUT 0x010, OUTPUT 0x014,
 * H/V coeff 0x018/0x01c, luma offsets 0x020..0x02c) per the vendor ddp_rsz.c. */
#define RSZ0_R(off)    (*(volatile unsigned int *)(0x1401A000u + (off)))
#define OVL0_2L_ROI    (*(volatile unsigned int *)(0x14009000u + 0x020u))
#define OVL0_2L_BGCLR  (*(volatile unsigned int *)(0x14009000u + 0x028u)) /* ROI_BGCLR (bars) */
#define OVL0_2L_L0OFF  (*(volatile unsigned int *)(0x14009000u + 0x03cu)) /* L0 dst offset y<<16|x */

/* Vendor coeff-step + init-phase math (ddp_rsz.c rsz_calc_tile_params, single-tile). UNIT is
 * the 15-bit subpixel unit. Produces the polyphase step and the luma integer/subpixel offset
 * that aligns the first output sample. */
#define RSZ_UNIT 32768u
static void rsz_calc(unsigned in_len, unsigned out_len, unsigned *pstep, unsigned *pint, unsigned *psub)
{
	unsigned step, offset0, init_phase, int_off;
	if (out_len <= 1u || in_len <= 1u) { *pstep = RSZ_UNIT; *pint = 0; *psub = 0; return; }
	step = (RSZ_UNIT * (in_len - 1u) + (out_len - 2u)) / (out_len - 1u);
	offset0 = (step * (out_len - 1u) - RSZ_UNIT * (in_len - 1u)) / 2u;
	init_phase = RSZ_UNIT - offset0;
	int_off = init_phase / RSZ_UNIT;
	*pstep = step; *pint = int_off; *psub = init_phase - RSZ_UNIT * int_off;
}

static void snes_rsz_program(unsigned iw, unsigned ih, unsigned ow, unsigned oh)
{
	unsigned hstep, hint, hsub, vstep, vint, vsub;
	rsz_calc(iw, ow, &hstep, &hint, &hsub);
	rsz_calc(ih, oh, &vstep, &vint, &vsub);
	RSZ0_R(0x000) = 0;                                       /* ENABLE off while reprogramming */
	RSZ0_R(0x004) = ((iw != ow) ? 1u : 0u) | ((ih != oh) ? 2u : 0u); /* CONTROL_1: H_EN|V_EN */
	RSZ0_R(0x008) = (1u << 9) | (1u << 28);                  /* CONTROL_2: RGB888 (pwr_sv|rgb_bit) */
	RSZ0_R(0x010) = (ih << 16) | iw;                         /* INPUT_IMAGE  = h<<16 | w */
	RSZ0_R(0x014) = (oh << 16) | ow;                         /* OUTPUT_IMAGE = h<<16 | w */
	RSZ0_R(0x018) = hstep;                                   /* HORIZONTAL_COEFF_STEP */
	RSZ0_R(0x01c) = vstep;                                   /* VERTICAL_COEFF_STEP */
	RSZ0_R(0x020) = hint;                                    /* LUMA_HORIZONTAL_INTEGER_OFFSET */
	RSZ0_R(0x024) = hsub;                                    /* LUMA_HORIZONTAL_SUBPIXEL_OFFSET */
	RSZ0_R(0x028) = vint;                                    /* LUMA_VERTICAL_INTEGER_OFFSET */
	RSZ0_R(0x02c) = vsub;                                    /* LUMA_VERTICAL_SUBPIXEL_OFFSET */
	RSZ0_R(0x044) = 0x3;                                     /* DEBUG_SEL (vendor rsz_start) */
	RSZ0_R(0x000) = 0x1;                                     /* ENABLE: FLD_RSZ_EN */
	/* Commit the working registers to the active (shadow) copy so scanout uses them. Vendor
	 * rsz_start pulses FORCE_COMMIT (bit1 of SHADOW_CTRL) rather than bypassing shadow. */
	RSZ0_R(0x0f0) = 0x2;
	g_snes_rsz_dbg = RSZ0_R(0x000);
}

/* s_rsz_setup is forward-declared (tentative static) up near ayaneo_snes_rsz_restore's decl. */
static unsigned s_rsz_sw, s_rsz_sh;   /* source geometry the path was configured for */
static unsigned s_rsz_dw, s_rsz_dh;  /* output rect at last setup (source+rect key the reprogram) */
volatile unsigned g_rsz_show_us;      /* generic RSZ present cost (us), for oem diag */
/* oem diag showparts: the RSZ present split in us (overdrive, turn, prescale, hud, cache clean, layer config) */
volatile unsigned g_dbg_show_parts[6];
volatile unsigned int g_dbg_pace_last_ticks;   /* last present's pace wait (13 MHz ticks) */

/* ---- hardware overlay layer (foundation for the live-menu-over-running-game feature) ----------
 * The menu will be composited by the OVL hardware as a SEPARATE layer over the game, so the game
 * keeps running through its normal path (RSZ + run-ahead) with zero CPU composite cost. The game
 * is on OVL0_2L L0 (global layer 0) -> RSZ -> OVL0 background; OVL0's own 4 layers (global 2..5)
 * are free, so the overlay goes on OVL0 L0 = global layer 2, composited AFTER RSZ (not scaled),
 * with per-pixel alpha (transparent outside the panel). config_input writes ovl_config[layer], so
 * layer=2 targets OVL0 L0. Disable is a direct SRC_CON poke (config_input skips layer_en==0).
 * OVL0 base 0x14008000: SRC_CON @0x02C (L0_EN bit0). This is the de-risk test path first. */
#define OVL0_SRC_CON (*(volatile unsigned int *)(0x14008000u + 0x02Cu))
void ayaneo_overlay_layer_set(unsigned int argb_pa, unsigned int w, unsigned int h,
			      unsigned int dst_x, unsigned int dst_y, int enable)
{
	/* A layer config written at an arbitrary point of the scan shows a frame or two of torn,
	 * tiled garbage (CMDQ is off: the registers change mid frame). Land it right after an
	 * RDMA0 frame end, the one phase the presenter uses for its own configs. */
	{ extern int ayaneo_rdma_wait_frame_end(unsigned int timeout_us); ayaneo_rdma_wait_frame_end(30000u); }
	if (enable && argb_pa && w && h) {
		disp_input_config in;
		memset(&in, 0, sizeof(in));
		in.layer = 2;                 /* global layer 2 = OVL0 L0 (over the RSZ background) */
		in.layer_en = 1;
		in.fmt = eBGRA8888;           /* per-pixel alpha */
		in.addr = argb_pa;
		in.src_x = 0; in.src_y = 0; in.src_w = w; in.src_h = h; in.src_pitch = w * 4u;
		in.dst_x = dst_x; in.dst_y = dst_y; in.dst_w = w; in.dst_h = h;
		in.aen = 1; in.alpha = 0xff;  /* aen + ARGB = per-pixel alpha blend over the game */
		primary_display_config_input(&in);
		primary_display_trigger(1);
	} else {
		/* Disable via config_input(layer_en=0), NOT a bare SRC_CON poke: OVLConfig rebuilds OVL0
		 * SRC_CON from the cached ovl_config[].layer_en on EVERY config_input, so the game's next
		 * per-frame present would re-enable a poked-off layer (the flicker-back bug). Clearing the
		 * cached layer_en makes the disable persist; OVLConfig then drops it from enabled_layers. */
		disp_input_config in;
		memset(&in, 0, sizeof(in));
		in.layer = 2; in.layer_en = 0;
		primary_display_config_input(&in);
		primary_display_trigger(1);
	}
}

/* oem ovltest:N validation hook: fill a small semi-transparent square at 0x55800000 (free during
 * a game) and composite it as OVL0 L0 over the running game, or disable. Proves the hardware
 * overlay + alpha + topology before the full menu renderer is wired to it. */
void ayaneo_overlay_test(int on)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	const unsigned int TW = 384, TH = 384;
	unsigned int *b = (unsigned int *)0x55800000u;
	if (on) {
		unsigned int y, x;
		for (y = 0; y < TH; y++)
			for (x = 0; x < TW; x++) {
				/* border opaque red, interior 50% blue - so we can see both the layer AND the
				 * game showing through the alpha. */
				int edge = (x < 8 || x >= TW - 8 || y < 8 || y >= TH - 8);
				b[y * TW + x] = edge ? 0xFFFF0000u : 0x800000FFu;
			}
		arch_clean_cache_range(0x55800000u, TW * TH * 4u);
		ayaneo_overlay_layer_set(0x55800000u, TW, TH, (W - TW) / 2u, (H - TH) / 2u, 1);
	} else {
		ayaneo_overlay_layer_set(0, 0, 0, 0, 0, 0);
	}
}

/* ---- live in-game menu as a hardware overlay layer -------------------------------------------
 * The Pico menu is rendered into a full-panel ARGB buffer (transparent everywhere the menu does
 * not draw, opaque where it does) and composited as OVL0 L0 over the RUNNING game. The game keeps
 * presenting through its normal path (RSZ/CPU) with the game changing live underneath, so aspect/
 * filter/etc. preview in real time with no game pause and zero CPU composite cost. The overlay
 * buffer is 0x55900000 (the "game-full" transition buffer, free during play); while the overlay
 * is up g_overlay_active makes the RSZ path single-buffer at 0x55000000 so the two never collide.
 * Enable the layer ONCE on open (the game's per-frame config_input keeps it in SRC_CON); just
 * refresh the buffer content each frame for live menu values (the OVL re-scans it every vsync). */
#define MENU_OVERLAY_PA 0x55900000u
volatile int g_overlay_active;
static volatile int s_overlay_dirty;
/* Cores call this whenever the menu CONTENT changes (open, navigation, value edit, volume rocker),
 * so the overlay is repainted only then - NOT every frame. Repainting every frame meant a
 * full-screen 4.9 MB clear + 4.9 MB cache-flush per frame; on the lower-clock GBA/GBC that overran
 * the frame budget, so the panel scanned a half-cleared overlay = stale content torn across the top.
 * The game still animates live underneath through its own present; only the menu panel is static. */
void ayaneo_menu_overlay_mark_dirty(void) { s_overlay_dirty = 1; }
/* Power hold bar: while the power key is held the overlay layer shows a "Hold to power off" bar
 * (unless a core's own menu owns the layer). -1 = no bar. */
static volatile int g_power_bar_pct = -1;
extern int ayaneo_text(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int scale, unsigned int argb, const char *s);
static void power_bar_draw(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H, int onto_canvas)
{
	int w = (int)W * 2 / 3, x0 = ((int)W - w) / 2, y0 = (int)H / 2 + 30, pct = g_power_bar_pct < 0 ? 0 : g_power_bar_pct;
	const char *t = "Hold to power off";
	int n = 0; while (t[n]) n++;
	/* dark scrim over the whole screen: alpha on the overlay layer, blended into an opaque canvas */
	if (onto_canvas) ayaneo_fill_blend(buf, pitch, 0, 0, (int)W, (int)H, 0xFF000000u, 208);
	else             ayaneo_fill(buf, pitch, 0, 0, (int)W, (int)H, 0xD0000000u);
	ayaneo_text(buf, pitch, (int)W / 2 - n * 8 * 4 / 2, y0 - 80, 4, 0xFFFFFFFFu, t);
	ayaneo_fill(buf, pitch, x0, y0, w, 20, 0xFF303840u);
	ayaneo_fill(buf, pitch, x0, y0, w * (pct > 100 ? 100 : pct) / 100, 20, 0xFFFFFFFFu);
}
static void power_bar_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H) { power_bar_draw(buf, pitch, W, H, 0); }
/* the carousel draws the bar straight into its canvas every frame (its present path owns the
 * OVL layers, so the overlay layer is not used there); no-op while no hold is in progress */
void ayaneo_power_bar_draw(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	if (g_power_bar_pct >= 0) power_bar_draw(buf, pitch, W, H, 1);
}
void ayaneo_power_bar(int pct)
{
	if (pct >= 0) pct = (pct / 5) * 5;   /* repaint in 5 % steps: the layer is re-turned on every repaint */
	if (pct != g_power_bar_pct) { g_power_bar_pct = pct; s_overlay_dirty = 1; }
}
/* oem gray:LEVEL - a flat opaque field over the whole screen on the overlay layer (panel
 * experiments: VCOM mistune shows as flicker on mid gray). 0 clears it. In game only: the
 * carousel owns its layers itself. */
volatile unsigned int g_test_fill_argb;
volatile int g_test_motion;   /* a yellow square with a black outline sweeping side to side (trail check) */
static void test_fill_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	unsigned int y, x, c = g_test_fill_argb;
	for (y = 0; y < H; y++) { unsigned int *o = buf + y * pitch; for (x = 0; x < W; x++) o[x] = c; }
	if (g_test_motion) {
		/* 120 px square, 4 px outline, one full sweep (there and back) every 1.5 s on the 13 MHz clock */
		int sq = (int)(H / 5u), st = 4, span = (int)W - sq;
		unsigned int t = gpt4_get_current_tick() / 13000u;          /* ms */
		unsigned int ph = t % 1500u;
		int px = (int)((ph < 750u ? ph : 1500u - ph) * (unsigned)span / 750u);
		int py = (int)(H - sq) / 2;
		ayaneo_fill(buf, pitch, px, py, sq, sq, 0xFF000000u);
		ayaneo_fill(buf, pitch, px + st, py + st, sq - 2 * st, sq - 2 * st, 0xFFFFE000u);
	}
}
void ayaneo_test_fill(unsigned int argb, int motion) { g_test_fill_argb = argb; g_test_motion = argb ? motion : 0; s_overlay_dirty = 1; }
void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int *b = (unsigned int *)MENU_OVERLAY_PA;
	static unsigned int s_overlay_paint_tick;
	(void)test_fill_paint;   /* the test field is drawn into the game frame now (ayaneo_rsz_present), so the overdrive sees it */
	if (!(open && paint) && g_power_bar_pct >= 0) { paint = power_bar_paint; open = 1; }   /* the bar borrows the layer */
	if (open && paint) {
		/* live rows (battery, clock, refresh, fps): repaint twice a second while open, not only
		 * on a key press, so the figures do not go stale until the menu is reopened */
		if (g_overlay_active && gpt4_get_current_tick() - s_overlay_paint_tick >= 6500000u) s_overlay_dirty = 1;
		if (!g_overlay_active || s_overlay_dirty) {
			s_overlay_paint_tick = gpt4_get_current_tick();
			/* Clear to transparent (alpha=0) then draw the opaque panel. Full-screen clear each
			 * repaint handles GBA/GBC swapping between differently-sized panels (menu vs palette/
			 * aspect pickers) with no stale opaque pixels - but only on a real content change.
			 * Painted BEFORE the layer is enabled on the open frame so no stale buffer flashes. */
#if AYANEO_ROT
			{
				unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h();
				unsigned int *lb = (unsigned int *)ROT_OVERLAY_PA;
				memset(lb, 0, LW * LH * 4u);
				ayaneo_draw_clip(LW, LH);
				paint(lb, LW, LW, LH);
				ayaneo_draw_clip(0, 0);
				ayaneo_rot8888_alpha_neon(lb, LW, LH, LW, b, W, g_rot_ccw);   /* keep the alpha: the game shows through */
			}
#else
			memset(b, 0, W * H * 4u);
			paint(b, W, W, H);
#endif
			arch_clean_cache_range(MENU_OVERLAY_PA, W * H * 4u);
			s_overlay_dirty = 0;
		}
		if (!g_overlay_active) {
			g_overlay_active = 1;
			ayaneo_overlay_layer_set(MENU_OVERLAY_PA, W, H, 0, 0, 1);
		}
	} else if (g_overlay_active) {
		ayaneo_overlay_layer_set(0, 0, 0, 0, 0, 0);
		g_overlay_active = 0;
	}
}

/* Undo the RSZ path: disable the resizer and restore the OVL0_2L ROI to full panel, so the
 * menu / other cores render correctly after an RSZ SNES session. Called at SNES session exit
 * and by oem snes-rsz:0. Also clears the config-once latch so the next session reconfigures. */
void ayaneo_snes_rsz_restore(void)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	ayaneo_menu_overlay(0, 0);   /* also drop the menu overlay layer, so the carousel/reverse-punch
				      * transition is clean (catch-all: exit, oem toggle, canvas self-heal) */
	/* Reset the resizer to a clean 1:1 PANEL passthrough, not just "enable off": leaving the
	 * session's INPUT_IMAGE (e.g. 1024x896) in place while the OVL0_2L feeds a full 1280x960
	 * frame makes the disabled resizer mismatch its input and stall the pipe (no frame-done ->
	 * the menu present blocks -> the Close hang). Program input == output == panel, H/V scale
	 * off, then disable, and force-commit. */
	RSZ0_R(0x004) = 0;                  /* CONTROL_1: H_EN=0, V_EN=0 */
	RSZ0_R(0x010) = (H << 16) | W;      /* INPUT_IMAGE  = panel */
	RSZ0_R(0x014) = (H << 16) | W;      /* OUTPUT_IMAGE = panel */
	RSZ0_R(0x000) = 0;                  /* RSZ_ENABLE off (pass-through) */
	RSZ0_R(0x0f0) = 0x2;                /* FORCE_COMMIT so it reaches the active copy immediately */
	OVL0_2L_ROI   = (H << 16) | W;
	OVL0_2L_L0OFF = 0;   /* clear the centred-layer offset the RSZ path left */
	/* Deterministically re-commit the default full-panel layer (not just the ROI) so the menu
	 * renders 1:1 regardless of what the session left in the layer registers. Skip the video-mode
	 * frame-done wait inside config_input for this one reconfigure: the RSZ session drove the pipe
	 * with a small ROI and per-frame configs, and blocking on frame-done here can stall the caller
	 * (the SNES exit thread) so it never signals the menu thread = the Close hang, and leaves the
	 * OVL half-reconfigured = the menu corruption. The trigger commits the config either way. */
	if (fb_addr_pa) {
		extern int ayaneo_present_skip_framedone;
		int save_skip = ayaneo_present_skip_framedone;
		disp_input_config in;
		memset(&in, 0, sizeof(in));
		in.layer = FB_LAYER; in.layer_en = 1;
		in.fmt = redoffset_32bit ? eBGRA8888 : eRGBA8888;
		in.addr = (unsigned int)fb_addr_pa;
		in.src_x = 0; in.src_y = 0; in.src_w = W; in.src_h = H;
		in.src_pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT) * 4;
		in.dst_x = 0; in.dst_y = 0; in.dst_w = W; in.dst_h = H;
		in.aen = 1; in.alpha = 0xff;
		ayaneo_present_skip_framedone = 1;
		primary_display_config_input(&in);
		primary_display_trigger(1);
		ayaneo_present_skip_framedone = save_skip;
		OVL0_2L_ROI   = (H << 16) | W;   /* re-assert post-commit (direct write is live) */
		OVL0_2L_L0OFF = 0;
	}
	s_rsz_setup = 0;
}

/* Integer-prescale + RSZ fractional-finish present, with centering.
 *
 * The pipeline is OVL0_2L -> RSZ0 -> OVL0. RSZ scales its input uniformly from the top-left
 * origin, so it cannot by itself place a scaled game centred with black bars. The trick that
 * avoids poking OVL0's compositor (which crashed the device in earlier bring-up) is to do the
 * centring on the OVL0_2L INPUT side: give OVL0_2L a PADDED ROI (W' x H') with a black
 * background, place the integer-prescaled game (IW x IH) at an offset (ox, oy) inside it, then
 * let RSZ scale the whole padded frame W' x H' -> panel W x H. Because RSZ scales uniformly,
 * the game lands at (ox*W/W', oy*H/H') with size (IW*W/W', IH*H/H') and the black padding
 * scales to the surrounding bars. Choosing W' = IW*W/dw and ox = xoff*W'/W (and likewise for
 * height) makes that final rect exactly (xoff, yoff, dw, dh).
 *
 * Sharpness: the game is first prescaled by an INTEGER factor (nearest, crisp). RSZ then only
 * does the sub-2x residual (e.g. 4:3 is a 1.17x horizontal finish; Pixel and Stretch-horizontal
 * are exactly 1.0x = no blur at all). Only the fractional residual is bilinear.
 *
 * Double-buffered via the CPU path's framebuffer (fb_addr / fb_addr_pa), which is idle during
 * an RSZ session and proven DMA-visible; flipping s_fb_flip each frame stops the OVL from
 * scanning a half-written buffer (the transition garble). primary_display_config_input runs
 * every frame: in video mode it waits frame-done before latching the new buffer, which both
 * paces the loop (no fast-forward) and hands the buffer over cleanly at the frame boundary. */
/* Core-agnostic hardware-RSZ present: scale source (sw x sh, RGB565) to the rect (dw x dh) at
 * (xoff, yoff) on the 1280x960 panel via OVL0_2L -> RSZ0 -> OVL0. The source is integer-prescaled
 * (crisp nearest) into a padded OVL0_2L input ROI with a black background, then RSZ uniformly
 * scales the whole padded frame to the panel, so the game lands exactly at (xoff, yoff, dw, dh)
 * with letterbox bars - centring falls out of the geometry, no OVL0 compositor pokes. Only the
 * sub-integer residual is bilinear ("sharp bilinear"); integer-exact axes pass through untouched.
 * filt = LCD filter level; wait_vsync=0 skips the vsync wait (benchmark/uncapped). Callers pass a
 * target rect from their OWN aspect options; PIXEL-PERFECT (integer) modes should use the CPU
 * integer blit instead (sharper, and RSZ 1:1 buys nothing). One core runs at a time, so the
 * geometry-tracking statics are shared safely. */
/* ---- software overdrive / RTC (motion-trail reduction) --------------------------------------
 * True per-transition overdrive done in SOFTWARE, using the emulator's PREVIOUS frame (which the
 * SoC scanout does not retain). For each native pixel, drive it PAST its target in the direction it
 * just moved - out = clamp(cur + k/16 * (cur - prev)) per RGB565 channel - so the slow LC reaches
 * the real target within the frame instead of trailing. Static pixels (delta 0) are untouched.
 * Zero net DC (per-frame image content, not a standing bias) so no retention / panel wear. Runs at
 * native res (<1 ms) before the hardware upscale. Overshoot = k/16 of the frame-to-frame delta, split
 * into rising / falling (LC is direction-asymmetric) with a small-delta threshold and a per-channel cap.
 * The RISING gain is NOT a scalar but a table indexed by the ORIGIN (prev) level: this st7703's slow
 * rises are dark-origin, while over-driving already-bright rises is exactly the halo, so gain runs high
 * at prev=black (kuphi, the smear killer) and low at prev=white (kuplo, the proven halo-free floor).
 * Falling stays flat (empirically halo-free). Tunable live via `oem od:ON,KUP,KDN,THR,CAP` (flattens the
 * rising curve to KUP == old behavior) and `oem odup:KUPHI,KUPLO,GAM` (the prev-indexed rising curve). */
static volatile int   g_od_on = 1;    /* default ON with the tuned profile below; persisted setting overrides at load */
static volatile int   g_od_kdn = 6;   /* FALLING overshoot (flat) - "free" (no visible halo on this panel) */
static volatile int   g_od_thr = 3;   /* skip overshoot when |delta| <= thr (kills small-change / gradient halos) */
static volatile int   g_od_cap = 3;   /* max |overshoot| per 5-bit channel (0 = uncapped). Bounds the halo on
                                       * HIGH-contrast edges (biggest k*delta, worst fringe, fastest native LC). */
/* Prev-indexed RISING gain: keff runs from kuphi at prev=black down to kuplo at prev=white, bent by gam
 * (q4, 16 = linear; higher collapses the boost toward the darkest origins sooner). Rebuilt off the hot
 * path into g_od_up5/up6 on any knob change. Flat (kuphi==kuplo) is byte-identical to the old scalar. */
static volatile int   g_od_kuphi = 6; /* rising gain /16 at prev=BLACK (dark-origin smear killer) */
static volatile int   g_od_kuplo = 2; /* rising gain /16 at prev=WHITE (halo-free floor) */
static volatile int   g_od_gam   = 22;/* rising falloff shape: 16 linear, higher = faster collapse to kuplo */
static volatile int   g_od_capup = 3; /* separate rising / falling caps (oem odcap); ayaneo_set_overdrive sets both */
static volatile int   g_od_capdn = 3;
static volatile int   g_od_head = 0;  /* rail headroom in 5-bit levels: the picture is mapped into [head, max-head]
                                       * so a transition to full black or full colour can still be overshot */
static unsigned char  g_od_up5[32];   /* rising gain per 5-bit origin level (R/B) */
static unsigned char  g_od_up6[64];   /* rising gain per 6-bit origin level (G)   */
static volatile int   g_od_need_build = 1;    /* rebuild the tables at next od_apply */
static unsigned short s_od_prev[512 * 512];   /* previous native frame, packed at sw stride */
static unsigned short s_od_out[512 * 512];    /* overshot output handed to the resizer */
static unsigned int   s_od_pw, s_od_ph;       /* dims of the stored previous frame */
/* The whole overdrive as two tables, rebuilt with the gain tables: the output level for every
 * (previous, current) pair, [p << 5 | c] for the 5 bit red and blue, [p << 6 | c] for the 6 bit
 * green. Three loads per pixel instead of od_ch's arithmetic three times; same values exactly. */
static unsigned char  s_od5[32 * 32];
static unsigned char  s_od6[64 * 64];
volatile int g_dbg_od_lut = 1;   /* oem perf bit 0: 0 = the arithmetic per pixel path, for A/B timing */
volatile int g_dbg_plain_neon = 1;   /* oem perf bit 1: NEON plain prescale (no LCD filter) */
volatile int g_dbg_filt_ovr = -1;   /* oem perf third value: runtime LCD filter for the game picture, -1 = setting */
volatile int g_dbg_lcd_neon8 = 1;    /* oem perf bit 3: LCD filters in 8 bit lanes */
volatile int g_dbg_od_pair = 1;      /* oem perf bit 5: overdrive two pixels a step */

/* Fill a rising-gain table t[0..maxv]: lo..hi across origin darkness, gam bends linear<->squared. */
static void od_build_up(unsigned char *t, int maxv, int hi, int lo, int gam)
{
	int p;
	for (p = 0; p <= maxv; p++) {
		int frac = ((maxv - p) * 256) / maxv;          /* 256 at prev=black, 0 at white */
		int wf = frac;
		if (gam != 16) { int sq = (frac * frac) >> 8;  /* blend linear<->squared by (gam-16) */
				 wf = frac + (((sq - frac) * (gam - 16)) >> 4); }
		if (wf < 0) wf = 0; if (wf > 256) wf = 256;
		t[p] = (unsigned char)(lo + (((hi - lo) * wf) >> 8));   /* lo..hi, monotone in darkness */
	}
}
static void od_build_lut(void);
static void od_rebuild(void)
{
	od_build_up(g_od_up5, 31, g_od_kuphi, g_od_kuplo, g_od_gam);
	od_build_up(g_od_up6, 63, g_od_kuphi, g_od_kuplo, g_od_gam);
	g_od_need_build = 0;
	od_build_lut();
}

/* Scalar entry (legacy `oem od:` + the off path): flattens the rising curve to KUP so it reproduces the
 * old behavior exactly; the prev-indexed curve is set separately by ayaneo_set_overdrive_curve. */
void ayaneo_set_overdrive(int on, int kup, int kdn, int thr, int cap)
{
	if (kup < 0) kup = 0; else if (kup > 64) kup = 64;
	if (kdn < 0) kdn = 0; else if (kdn > 64) kdn = 64;
	if (thr < 0) thr = 0; else if (thr > 16) thr = 16;
	if (cap < 0) cap = 0; else if (cap > 31) cap = 31;
	g_od_kuphi = kup; g_od_kuplo = kup;   /* flat rising == scalar */
	g_od_kdn = kdn; g_od_thr = thr; g_od_cap = cap; g_od_capup = cap; g_od_capdn = cap;
	g_od_need_build = 1;
	g_od_on = on ? 1 : 0;
	if (!g_od_on) { s_od_pw = 0; s_od_ph = 0; }   /* drop history so a re-enable starts clean */
}

/* Set the prev-indexed RISING curve (leaves on/kdn/thr/cap): kuphi = dark-origin gain, kuplo =
 * bright-origin gain, gam = falloff shape. */
/* oem odcap:UP,DN,HEAD - rising cap, falling cap (5-bit levels, 0 = uncapped), rail headroom 0..3 */
void ayaneo_set_overdrive_caps(int up, int dn, int head)
{
	if (up < 0) up = 0; else if (up > 31) up = 31;
	if (dn < 0) dn = 0; else if (dn > 31) dn = 31;
	if (head < 0) head = 0; else if (head > 3) head = 3;
	g_od_capup = up; g_od_capdn = dn; g_od_head = head;
	g_od_need_build = 1;   /* the per pair tables hold the caps and headroom */
}
void ayaneo_get_overdrive_caps(int *up, int *dn, int *head) { *up = g_od_capup; *dn = g_od_capdn; *head = g_od_head; }

void ayaneo_set_overdrive_curve(int kuphi, int kuplo, int gam)
{
	if (kuphi < 0) kuphi = 0; else if (kuphi > 64) kuphi = 64;
	if (kuplo < 0) kuplo = 0; else if (kuplo > 64) kuplo = 64;
	if (gam < 8) gam = 8; else if (gam > 48) gam = 48;
	g_od_kuphi = kuphi; g_od_kuplo = kuplo; g_od_gam = gam;
	g_od_need_build = 1;
}

static inline int od_ch(int c, int p, int maxv, int scale, const unsigned char *uptab)
{
	int d = c - p;
	int ad = d < 0 ? -d : d;
	int ov, r, k, cap, h = g_od_head * scale;
	int cu = g_od_capup * scale, cd = g_od_capdn * scale;
	k = (d > 0) ? uptab[p] : g_od_kdn;                     /* RISING gain from ORIGIN(prev); FALLING flat */
	cap = (d > 0) ? cu : cd;
	if (h) c = h + (c * (maxv - 2 * h) + maxv / 2) / maxv; /* headroom: the steady picture sits inside the rails */
	if (ad <= g_od_thr) return c;                          /* tiny change: leave it, no halo */
	ov = (k * d) / 16;
	if (h) ov = (ov * (maxv - 2 * h)) / maxv;
	if (cap > 0) { if (ov > cap) ov = cap; else if (ov < -cap) ov = -cap; }   /* bound the halo on big deltas */
	r = c + ov;
	if (r < 0) return 0;
	if (r > maxv) return maxv;
	return r;
}

static inline unsigned short od_pixel(unsigned short c, unsigned short p)
{
	/* caps and headroom are in 5-bit units; green (6-bit) gets 2x to match */
	int r = od_ch((c >> 11) & 0x1F, (p >> 11) & 0x1F, 31, 1, g_od_up5);
	int g = od_ch((c >> 5) & 0x3F, (p >> 5) & 0x3F, 63, 2, g_od_up6);
	int b = od_ch(c & 0x1F, p & 0x1F, 31, 1, g_od_up5);
	return (unsigned short)((r << 11) | (g << 5) | b);
}

static void od_build_lut(void)
{
	int p, c;
	for (p = 0; p < 32; p++)
		for (c = 0; c < 32; c++) s_od5[(p << 5) | c] = (unsigned char)od_ch(c, p, 31, 1, g_od_up5);
	for (p = 0; p < 64; p++)
		for (c = 0; c < 64; c++) s_od6[(p << 6) | c] = (unsigned char)od_ch(c, p, 63, 2, g_od_up6);
}

static inline unsigned short od_pixel_lut(unsigned c, unsigned p)
{
	return (unsigned short)((s_od5[((p >> 6) & 0x3E0u) | (c >> 11)] << 11) |
				(s_od6[((p << 1) & 0xFC0u) | ((c >> 5) & 0x3Fu)] << 5) |
				 s_od5[((p & 0x1Fu) << 5) | (c & 0x1Fu)]);
}

static int __attribute__((unused)) od_active(unsigned int sw, unsigned int sh)
{
	return g_od_on && !(g_od_kuphi <= 0 && g_od_kuplo <= 0 && g_od_kdn <= 0) && sw <= 512u && sh <= 512u;
}

#if AYANEO_ROT
/* Overdrive and the quarter turn in one pass: the overshot frame is written straight into the
 * turned buffer (sh x sw, pitch sh) that rot565 would have produced from od_apply's output.
 * The previous frame stays in source order. Same pixels as od_apply + rot565. */
static void od_apply_rot(const unsigned short *pix, unsigned int sw, unsigned int sh, unsigned int spitch_px,
			 unsigned short *dst)
{
	unsigned int x, y;
	int have_prev;
	if (g_od_need_build) od_rebuild();
	have_prev = (s_od_pw == sw && s_od_ph == sh);
	/* row order (8 row bands writing 16 byte runs measured slower: 2.0 against 1.4 ms at 600 MHz) */
	for (y = 0; y < sh; y++) {
		const unsigned short *src = pix + y * spitch_px;
		unsigned short *prv = s_od_prev + y * sw;
		if (!g_rot_ccw) {
			unsigned short *dc = dst + (sh - 1u - y);
			if (have_prev && g_dbg_od_pair && !(((unsigned long)src | (unsigned long)prv) & 3u) && !(sw & 1u)) {
				/* two pixels a step from 32 bit loads: more independent work for the in-order core */
				const unsigned int *s2 = (const unsigned int *)src;
				unsigned int *p2 = (unsigned int *)prv;
				unsigned short *d = dc;
				for (x = 0; x < sw; x += 2u, d += 2u * sh) {
					unsigned int c2 = *s2++, q2 = *p2;
					unsigned short o0 = od_pixel_lut(c2 & 0xFFFFu, q2 & 0xFFFFu);
					unsigned short o1 = od_pixel_lut(c2 >> 16, q2 >> 16);
					*p2++ = c2;
					d[0] = o0; d[sh] = o1;
				}
			} else if (have_prev)
				for (x = 0; x < sw; x++) { unsigned c = src[x]; dc[x * sh] = od_pixel_lut(c, prv[x]); prv[x] = (unsigned short)c; }
			else
				for (x = 0; x < sw; x++) { unsigned short c = src[x]; dc[x * sh] = c; prv[x] = c; }
		} else {
			unsigned short *dc = dst + (sw - 1u) * sh + y;
			if (have_prev)
				for (x = 0; x < sw; x++) { unsigned c = src[x]; dc[-(int)(x * sh)] = od_pixel_lut(c, prv[x]); prv[x] = (unsigned short)c; }
			else
				for (x = 0; x < sw; x++) { unsigned short c = src[x]; dc[-(int)(x * sh)] = c; prv[x] = c; }
		}
	}
	s_od_pw = sw; s_od_ph = sh;
}
#endif

/* Returns the buffer + stride to present: the overshot output when overdrive is active, or the
 * original frame unchanged (off, or the native resolution just changed so there is no valid prev). */
static const unsigned short *od_apply(const unsigned short *pix, unsigned int sw, unsigned int sh,
				      unsigned int spitch_px, unsigned int *out_pitch)
{
	unsigned int x, y;
	int have_prev;
	if (!g_od_on || (g_od_kuphi <= 0 && g_od_kuplo <= 0 && g_od_kdn <= 0) || sw > 512u || sh > 512u) return pix;
	if (g_od_need_build) od_rebuild();
	have_prev = (s_od_pw == sw && s_od_ph == sh);
	for (y = 0; y < sh; y++) {
		const unsigned short *src = pix + y * spitch_px;
		unsigned short *prv = s_od_prev + y * sw;
		unsigned short *dst = s_od_out + y * sw;
		if (have_prev)
			if (g_dbg_od_lut)
				for (x = 0; x < sw; x++) { unsigned c = src[x]; dst[x] = od_pixel_lut(c, prv[x]); prv[x] = (unsigned short)c; }
			else
				for (x = 0; x < sw; x++) { unsigned short c = src[x]; dst[x] = od_pixel(c, prv[x]); prv[x] = c; }
		else
			for (x = 0; x < sw; x++) { unsigned short c = src[x]; dst[x] = c; prv[x] = c; }
	}
	s_od_pw = sw; s_od_ph = sh;
	*out_pitch = sw;
	return s_od_out;
}

#if AYANEO_ROT
/* In-game OSD and FF/RW badge on the turned panel. The prescaled game in the resizer buffer is
 * physical (turned); the badge and the volume bar are drawn upright by pulling the small logical
 * box they cover out of that buffer, drawing with the ordinary helpers, and pushing the box back
 * turned. Boxes are a few hundred pixels wide, so this costs almost nothing per frame.
 * Logical prescaled game: LGW = ih (physical height), LGH = iw (physical width). */
#define ROT_BOX_W 640
#define ROT_BOX_H 64
static unsigned int s_rot_box[ROT_BOX_W * ROT_BOX_H];

static inline unsigned int *rot_game_px(unsigned int *vbuf, unsigned fbpitch, unsigned iw, unsigned ih,
					int x, int y)
{
	unsigned px, py;
	if (!g_rot_ccw) { px = iw - 1u - (unsigned)y; py = (unsigned)x; }
	else            { px = (unsigned)y;           py = ih - 1u - (unsigned)x; }
	return vbuf + py * fbpitch + px;
}

/* Pull / push walk the turned buffer in its own row order: one logical column of the box is one
 * physical row, and the box's logical rows are neighbouring pixels in it. The per pixel address
 * helper (rot_game_px) read the box a pixel per cache line, 1.1 ms a frame at 600 MHz for the
 * FPS badge alone. */
static void rot_box_xfer(unsigned int *vbuf, unsigned fbpitch, unsigned iw, unsigned ih,
			 int lx, int ly, int lw, int lh, int push)
{
	int x, y;
	for (x = 0; x < lw; x++) {
		unsigned int *b = s_rot_box + x;
		if (!g_rot_ccw) {
			/* logical (x, y) -> physical row x, column iw - 1 - y */
			unsigned int *r = vbuf + (unsigned)(lx + x) * fbpitch + (iw - 1u - (unsigned)ly);
			if (push) for (y = 0; y < lh; y++) r[-y] = b[y * lw];
			else      for (y = 0; y < lh; y++) b[y * lw] = r[-y];
		} else {
			/* logical (x, y) -> physical row ih - 1 - x, column y */
			unsigned int *r = vbuf + (ih - 1u - (unsigned)(lx + x)) * fbpitch + (unsigned)ly;
			if (push) for (y = 0; y < lh; y++) r[y] = b[y * lw];
			else      for (y = 0; y < lh; y++) b[y * lw] = r[y];
		}
	}
}

static void rot_box_pull(unsigned int *vbuf, unsigned fbpitch, unsigned iw, unsigned ih,
			 int lx, int ly, int lw, int lh)
{
	rot_box_xfer(vbuf, fbpitch, iw, ih, lx, ly, lw, lh, 0);
}

static void rot_box_push(unsigned int *vbuf, unsigned fbpitch, unsigned iw, unsigned ih,
			 int lx, int ly, int lw, int lh)
{
	rot_box_xfer(vbuf, fbpitch, iw, ih, lx, ly, lw, lh, 1);
}

static void rot_hud_osd(unsigned int *vbuf, unsigned fbpitch, unsigned iw, unsigned ih)
{
	int LGW = (int)ih, LGH = (int)iw;

	/* FPS badge: top left of the game; the box is the badge itself (ayaneo_hud_draw_fps puts it
	 * at 12, 12 of the game), nothing around it is touched */
	if (ayaneo_hud_fps_visible()) {
		int lw, lh, lx = 12, ly = 12;
		ayaneo_hud_fps_size(&lw, &lh);
		if (lx + lw > LGW) lw = LGW - lx;
		if (ly + lh > LGH) lh = LGH - ly;
		if (lw > ROT_BOX_W) lw = ROT_BOX_W;
		if (lh > ROT_BOX_H) lh = ROT_BOX_H;
		rot_box_pull(vbuf, fbpitch, iw, ih, lx, ly, lw, lh);
		ayaneo_draw_clip((unsigned)lw, (unsigned)lh);
		ayaneo_hud_draw_fps(s_rot_box, (unsigned)lw, -lx, -ly);
		ayaneo_draw_clip(0, 0);
		rot_box_push(vbuf, fbpitch, iw, ih, lx, ly, lw, lh);
	}
	/* FF / RW badge: top right of the game, as ayaneo_hud_draw lays it out */
	if (g_hud_mode == 1 || g_hud_mode == 2) {
		int lw = 332, lh = 68, lx, ly = 0;
		if (lw > LGW) lw = LGW;
		if (lh > LGH) lh = LGH;
		lx = LGW - lw;
		rot_box_pull(vbuf, fbpitch, iw, ih, lx, ly, lw, lh);
		ayaneo_draw_clip((unsigned)lw, (unsigned)lh);
		s_hud_no_fps = 1;
		ayaneo_hud_draw(s_rot_box, (unsigned)lw, -lx, -ly, LGW, LGH);
		s_hud_no_fps = 0;
		ayaneo_draw_clip(0, 0);
		rot_box_push(vbuf, fbpitch, iw, ih, lx, ly, lw, lh);
	}

	/* volume / brightness bar: bottom centre of the game, same look as ayaneo_draw_osd */
	if (s_osd_kind && (int)(s_osd_until_ms - (unsigned int)current_time()) > 0) {
		int barW = (LGW * 3) / 5, barH = 22, bx, by, fillW, x, y;
		unsigned int fill = (s_osd_kind == 1) ? 0xFF20D0FFu : 0xFFFFC020u;
		if (barW > ROT_BOX_W) barW = ROT_BOX_W;
		bx = (LGW - barW) / 2;
		by = LGH - barH - 12;
		if (by < 0) by = 0;
		fillW = ((barW - 4) * s_osd_pct) / 100;
		for (y = 0; y < barH; y++) {
			unsigned int *o = s_rot_box + y * barW;
			int edge_row = (y < 2 || y >= barH - 2);
			for (x = 0; x < barW; x++)
				o[x] = (edge_row || x < 2 || x >= barW - 2) ? 0xFFFFFFFFu
				     : ((x - 2 < fillW) ? fill : 0xFF202020u);
		}
		rot_box_push(vbuf, fbpitch, iw, ih, bx, by, barW, barH);
	}
}
#endif

/* LCD1x / LCD3x (RetroArch handheld shaders, Gigaherz / jdgleaver, public domain / GPL):
 *   lcd1x: colour * (16 + sin(2 pi (y - 0.25))) / 17 * (4 + sin(2 pi (x - 0.25))) / 5
 *   lcd3x: colour * (16 + sin(2 pi y)) / 17 * (4 + sin(2 pi x + off_c)) / 5, off = pi/2,
 *          pi/2 - 2pi/3, pi/2 - 4pi/3 for R, G, B (the colour separation)
 * with x, y in source pixel units. With an integer prescale of ph x pv the factors depend only
 * on the sub position, so one 8 bit table per (sub row, sub column, channel) does the whole
 * frame with one multiply per channel. On the turned panel the game's rows run along the
 * output columns, so the scanline axis and the LCD axis swap. */
static const short s_sin256[256] = { 0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,10278,11039,11793,12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,19519,20159,20787,21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,26790,27245,27683,28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,31356,31580,31785,31971,32137,32285,32412,32521,32609,32678,32728,32757,32767,32757,32728,32678,32609,32521,32412,32285,32137,31971,31785,31580,31356,31113,30852,30571,30273,29956,29621,29268,28898,28510,28105,27683,27245,26790,26319,25832,25329,24811,24279,23731,23170,22594,22005,21403,20787,20159,19519,18868,18204,17530,16846,16151,15446,14732,14010,13279,12539,11793,11039,10278,9512,8739,7962,7179,6393,5602,4808,4011,3212,2410,1608,804,0,-804,-1608,-2410,-3212,-4011,-4808,-5602,-6393,-7179,-7962,-8739,-9512,-10278,-11039,-11793,-12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,-18204,-18868,-19519,-20159,-20787,-21403,-22005,-22594,-23170,-23731,-24279,-24811,-25329,-25832,-26319,-26790,-27245,-27683,-28105,-28510,-28898,-29268,-29621,-29956,-30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,-32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,-32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,-32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,-30273,-29956,-29621,-29268,-28898,-28510,-28105,-27683,-27245,-26790,-26319,-25832,-25329,-24811,-24279,-23731,-23170,-22594,-22005,-21403,-20787,-20159,-19519,-18868,-18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,-12539,-11793,-11039,-10278,-9512,-8739,-7962,-7179,-6393,-5602,-4808,-4011,-3212,-2410,-1608,-804 };
static unsigned s_lcd_key;
static unsigned char s_lcd_m[8][8][3];   /* [sub row][sub col][channel], 255 = x1 */
static void lcd_filter_tables(int filt, unsigned ph, unsigned pv, int turned)
{
	unsigned j, k, c;
	unsigned key = ((unsigned)filt << 24) | (ph << 16) | (pv << 8) | (unsigned)turned;
	if (key == s_lcd_key) return;
	s_lcd_key = key;
	if (ph > 8u) ph = 8u;
	if (pv > 8u) pv = 8u;
	for (j = 0; j < pv; j++) {
		for (k = 0; k < ph; k++) {
			/* sub position centre in 1/256 turns along each axis */
			unsigned srow = (unsigned)(((2u * j + 1u) * 128u) / pv);   /* along output rows */
			unsigned scol = (unsigned)(((2u * k + 1u) * 128u) / ph);   /* along output columns */
			unsigned ys = turned ? scol : srow;   /* the game's vertical (scanline) axis */
			unsigned xs = turned ? srow : scol;   /* the game's horizontal (LCD column) axis */
			int fy, sy, fx[3];
			if (filt == 4) { ys = (ys + 256u - 64u) & 255u; xs = (xs + 256u - 64u) & 255u; }
			sy = s_sin256[ys & 255u];
			fy = (16 * 32767 + sy) / 17;   /* x 32767 */
			for (c = 0; c < 3; c++) {
				unsigned a = xs;
				if (filt == 5) a = (a + (c == 0 ? 64u : c == 1 ? (256u + 64u - 85u) : (256u + 64u - 171u))) & 255u;
				fx[c] = (4 * 32767 + s_sin256[a & 255u]) / 5;
				{
					long m = ((long)fy * (long)fx[c]) / 32767L;   /* x 32767 */
					m = (m * 255L) / 32767L;
					s_lcd_m[j][k][c] = (unsigned char)(m < 0 ? 0 : (m > 255 ? 255 : m));
				}
			}
		}
	}
}
static void lcd_filter_prescale(unsigned int *vbuf, unsigned fbpitch, const unsigned short *pix,
				unsigned sw, unsigned sh, unsigned spitch_px, unsigned ph, unsigned pv,
				int filt, const unsigned short *cc_lut, int turned)
{
	unsigned y, j, x, k;
	lcd_filter_tables(filt, ph, pv, turned);
#if defined(KONKR_KPA)
	if (ph >= 1u && ph <= 4u && pv <= 8u) {
		/* NEON: eight source pixels per step, the sub columns stored interleaved; the GBA colour
		 * correction (cc_lut) is applied into a row scratch first */
		extern void ayaneo_lcd_prescale_row_neon(const unsigned short *src, unsigned sw, unsigned int *dst, unsigned fbpitch,
							 unsigned ph, unsigned pv, const unsigned char (*mtab)[8][3]);
		extern void ayaneo_lcd_prescale_row_neon8(const unsigned short *src, unsigned sw, unsigned int *dst, unsigned fbpitch,
							  unsigned ph, unsigned pv, const unsigned char (*mtab)[8][3]);
		static unsigned short s_ccrow[1024];
		for (y = 0; y < sh; y++) {
			const unsigned short *srow = pix + y * spitch_px;
			if (cc_lut && sw <= 1024u) {
				for (x = 0; x < sw; x++) { unsigned v = srow[x]; s_ccrow[x] = cc_lut[(((v >> 12) & 0xFu) << 8) | (((v >> 7) & 0xFu) << 4) | ((v >> 1) & 0xFu)]; }
				srow = s_ccrow;
			}
			if (g_dbg_lcd_neon8 && ph != 3u)
				ayaneo_lcd_prescale_row_neon8(srow, sw, vbuf + (y * pv) * fbpitch, fbpitch, ph, pv, (const unsigned char (*)[8][3])s_lcd_m);
			else
				ayaneo_lcd_prescale_row_neon(srow, sw, vbuf + (y * pv) * fbpitch, fbpitch, ph, pv, (const unsigned char (*)[8][3])s_lcd_m);
		}
		return;
	}
#endif
	for (y = 0; y < sh; y++) {
		const unsigned short *srow = pix + y * spitch_px;
		for (j = 0; j < pv; j++) {
			unsigned int *drow = vbuf + (y * pv + j) * fbpitch;
			const unsigned char (*mj)[3] = s_lcd_m[j < 8u ? j : 7u];
			for (x = 0; x < sw; x++) {
				unsigned int v = srow[x];
				unsigned r, g, b;
				unsigned int *o = drow + x * ph;
				if (cc_lut) v = cc_lut[(((v >> 12) & 0xFu) << 8) | (((v >> 7) & 0xFu) << 4) | ((v >> 1) & 0xFu)];
				r = ((v >> 11) & 0x1fu) << 3; g = ((v >> 5) & 0x3fu) << 2; b = (v & 0x1fu) << 3;
				for (k = 0; k < ph; k++) {
					const unsigned char *m = mj[k < 8u ? k : 7u];
					o[k] = 0xFF000000u | (((r * m[0]) >> 8) << 16) | (((g * m[1]) >> 8) << 8) | ((b * m[2]) >> 8);
				}
			}
		}
	}
}

void ayaneo_rsz_present(const unsigned short *pix, unsigned int sw, unsigned int sh, unsigned int spitch_px,
			unsigned int dw, unsigned int dh, unsigned int xoff, unsigned int yoff,
			int filt, int wait_vsync, const unsigned short *cc_lut)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int t0 = gpt4_get_current_tick();

	if (!pix || !fb_addr || !sw || !sh) return;
	if (g_dbg_filt_ovr >= 0 && g_dbg_filt_ovr <= 5) filt = g_dbg_filt_ovr;
	if (g_test_fill_argb && sw <= 512u && sh <= 512u) {
		/* oem gray: replace the game picture, at its native size and before the overdrive, with a
		 * flat gray and four outlined boxes (red, green, blue, yellow) sweeping side to side in
		 * step, so the trails and the overdrive halos are judged on the real pipeline */
		static unsigned short s_test_px[512 * 512];
		unsigned int c = g_test_fill_argb, x, y, i;
		unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
		unsigned short bg = (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		static const unsigned short cols[4] = { 0xF800, 0x07E0, 0x001F, 0xFFE0 };
		for (y = 0; y < sh; y++) { unsigned short *o = s_test_px + y * sw; for (x = 0; x < sw; x++) o[x] = bg; }
		if (g_test_motion) {
			int sq = (int)sh / 8, gap = ((int)sh - 4 * sq) / 5, span = (int)sw - sq;
			unsigned int ph = (gpt4_get_current_tick() / 13000u) % 1500u;          /* ms in the sweep */
			int px = (int)((ph < 750u ? ph : 1500u - ph) * (unsigned)span / 750u);
			if (sq < 4) sq = 4;
			for (i = 0; i < 4; i++) {
				int py = gap + (int)i * (sq + gap), yy, xx;
				for (yy = 0; yy < sq; yy++) {
					unsigned short *o = s_test_px + (unsigned)(py + yy) * sw + px;
					for (xx = 0; xx < sq; xx++)
						o[xx] = (yy == 0 || yy == sq - 1 || xx == 0 || xx == sq - 1) ? 0x0000 : cols[i];
				}
			}
		}
		pix = s_test_px; spitch_px = sw;
	}
#if AYANEO_ROT
	int od_turned = 0;   /* overdrive and the turn done together into ROT_565_PA */
	if (g_dbg_od_lut && od_active(sw, sh) && sw * sh <= ROT_565_MAX_PX) {
		od_apply_rot(pix, sw, sh, spitch_px, (unsigned short *)ROT_565_PA);
		od_turned = 1;
	} else
#endif
	pix = od_apply(pix, sw, sh, spitch_px, &spitch_px);   /* software overdrive (no-op unless enabled) */
	unsigned int tp_od = gpt4_get_current_tick(), tp_rot = tp_od, tp_pre, tp_hud, tp_cln, tp_pace = 0;
	unsigned int p_od = (tp_od - t0) / 13u;
#if AYANEO_ROT
	/* Callers describe the picture on the logical landscape screen. Turn the native frame now,
	 * while it is small, and map the target rect onto the portrait frame buffer; everything
	 * below then works in physical coordinates exactly as on a landscape panel. */
	{
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), px, py, pw, ph2, t;
		if (sw * sh > ROT_565_MAX_PX) return;
		if (dw > LW) dw = LW; if (dh > LH) dh = LH;
		if (dw < 1u) dw = 1u; if (dh < 1u) dh = 1u;
		if (xoff + dw > LW) xoff = (LW - dw) / 2u;
		if (yoff + dh > LH) yoff = (LH - dh) / 2u;
		if (!od_turned) rot565(pix, sw, sh, spitch_px, (unsigned short *)ROT_565_PA);
		pix = (const unsigned short *)ROT_565_PA;
		t = sw; sw = sh; sh = t; spitch_px = sw;
		rot_rect(xoff, yoff, dw, dh, &px, &py, &pw, &ph2);
		xoff = px; yoff = py; dw = pw; dh = ph2;
		tp_rot = gpt4_get_current_tick();
	}
#endif
	if (dw > W) dw = W; if (dh > H) dh = H;
	if (dw < 1u) dw = 1u; if (dh < 1u) dh = 1u;
	if (xoff + dw > W) xoff = (W - dw) / 2u;
	if (yoff + dh > H) yoff = (H - dh) / 2u;

	/* Integer prescale factor: the largest integer whose product with the source stays within
	 * the target rect, so RSZ only ever upscales the sub-integer residual (<2x, mild blur).
	 * The OVL0_2L layer's RDMA reads at most OVL_2L_TILE_W pixels per row in a single tile (LK
	 * never sets up OVL tiling); a wider layer wraps to garbage past that column (the Stretch
	 * corruption). So cap the prescaled width there and let RSZ upscale the rest - e.g. Stretch
	 * drops from 5x (1280, over the limit) to 4x (1024) + a 1.25x RSZ finish. */
	#define OVL_2L_TILE_W 1024u
	unsigned int ph = dw / sw; if (ph < 1u) ph = 1u;
	while (sw * ph > OVL_2L_TILE_W && ph > 1u) ph--;
	unsigned int pv = dh / sh; if (pv < 1u) pv = 1u;
	unsigned int ih = sh * pv;   /* prescaled (crisp) game height */
	/* When the RSZ also scales VERTICALLY (prescaled height ih < target dh), the 2D resizer's
	 * input-width limit tightens to ~OVL_2L_TILE_W: SNES Stretch is clean at wp=1024, but GBC Fit
	 * at wp=1152 (also 2D) showed a stale-scratch right bar. So in the 2D case cap the horizontal
	 * prescale too, keeping wp = iw*W/dw <= OVL_2L_TILE_W. Horizontal-only modes (ih==dh, e.g. SNES
	 * 4:3 at wp=1098) are UNAFFECTED - they scale one axis and tolerate the wider input. */
	if (ih < dh)
		while (ph > 1u && (sw * ph * W) / dw > OVL_2L_TILE_W) ph--;
	unsigned int iw = sw * ph;   /* prescaled (crisp) game width */

	/* Padded OVL0_2L input geometry so a uniform RSZ scale to the panel reproduces (xoff,dw)
	 * and (yoff,dh). Guard the divides; round to nearest. */
	unsigned int wp = (dw ? (iw * W + dw / 2u) / dw : W);
	unsigned int hp = (dh ? (ih * H + dh / 2u) / dh : H);
	if (wp < iw) wp = iw; if (wp > W) wp = W;
	if (hp < ih) hp = ih; if (hp > H) hp = H;
	/* The OVL0_2L padded ROI = the RSZ input; that width/height must be EVEN or the resizer
	 * mishandles the trailing column/row and shows stale scratch (the GBC Fit purple right bar:
	 * wp landed on the odd 1153). iw/ih are always even (even source * integer prescale), so
	 * rounding wp/hp down to even keeps wp >= iw. Every working SNES mode already had even wp. */
	wp &= ~1u; hp &= ~1u;
	if (wp < iw) wp = iw; if (hp < ih) hp = ih;
	unsigned int ox = (W ? (xoff * wp + W / 2u) / W : 0u);
	unsigned int oy = (H ? (yoff * hp + H / 2u) / H : 0u);
	ox &= ~1u; oy &= ~1u;   /* even layer offset too, for the same reason */
	if (ox + iw > wp) ox = wp - iw;
	if (oy + ih > hp) oy = hp - ih;

	/* Double-buffered scratch in the two idle menu-transition buffers (reveal 0x55000000 and
	 * game-full 0x55900000, each fb-sized 4.9 MB, disjoint, both in the WB-mapped window). These
	 * are free during gameplay and only reused by the exit reverse-punch AFTER RSZ is torn down.
	 * Using dedicated buffers (NOT the CPU framebuffer) keeps the menu's fb pristine - sharing fb
	 * sheared the menu because the RSZ layer stride differed from the panel stride. */
	#define RSZ_BUF0 0x55000000u
	#define RSZ_BUF1 0x55900000u
	/* While the menu overlay owns RSZ_BUF1 (0x55900000), single-buffer at RSZ_BUF0 so the game
	 * present and the overlay never collide. The game is mostly behind the panel then, so the
	 * lost double-buffering (minor tearing) is not noticeable. */
	extern volatile int g_overlay_active;
	unsigned int pbuf  = g_overlay_active ? RSZ_BUF0 : (s_fb_flip ? RSZ_BUF1 : RSZ_BUF0);
	unsigned int *vbuf = (unsigned int *)pbuf;   /* identity-mapped in LK */

	/* Prescale at the panel pitch so the game area is a clean, consistent layout. The game
	 * occupies the left iw columns of each row; RSZ reads only src_w=iw. */
	unsigned int fbpitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);   /* = panel pitch in pixels (1280) */

	/* RGB565 -> ARGB8888 integer nearest prescale (ph x pv). Optional LCD filter: scanlines
	 * dim the last of each pv rows; grid (filt>=2) also dims the last of each ph columns.
	 * On the turned panel (AYANEO_ROT) this runs on the ROTATED frame: the game's horizontal
	 * scanlines are columns here, so "scanlines" dims the last of each ph columns instead.
	 * Filters 4 (LCD1x) and 5 (LCD3x) are the RetroArch handheld shaders as per sub pixel
	 * factor tables (lcd_filter_prescale below). */
	if (filt >= 4) {
		lcd_filter_prescale(vbuf, fbpitch, pix, sw, sh, spitch_px, ph, pv, filt, cc_lut, AYANEO_ROT ? 1 : 0);
	}
#if defined(KONKR_KPA)
	else if (filt == 0 && g_dbg_plain_neon && ph >= 1u && ph <= 4u) {
		extern void ayaneo_plain_prescale_row_neon(const unsigned short *src, unsigned sw, unsigned int *dst,
							   unsigned fbpitch, unsigned ph, unsigned pv);
		static unsigned short s_ccrow0[1024];
		unsigned int y, x;
		for (y = 0; y < sh; y++) {
			const unsigned short *srow = pix + y * spitch_px;
			if (cc_lut && sw <= 1024u) {
				for (x = 0; x < sw; x++) { unsigned v = srow[x]; s_ccrow0[x] = cc_lut[(((v >> 12) & 0xFu) << 8) | (((v >> 7) & 0xFu) << 4) | ((v >> 1) & 0xFu)]; }
				srow = s_ccrow0;
			}
			ayaneo_plain_prescale_row_neon(srow, sw, vbuf + (y * pv) * fbpitch, fbpitch, ph, pv);
		}
	}
#endif
	else {
		const int scan_cols = AYANEO_ROT ? 1 : 0;
		unsigned int y, x, k, r_;
		for (y = 0; y < sh; y++) {
			const unsigned short *srow = pix + y * spitch_px;
			unsigned int *drow = vbuf + (y * pv) * fbpitch;
			for (x = 0; x < sw; x++) {
				unsigned int v = srow[x];
				/* Optional GBA-style RGB444 colour-correction LUT (matches the CPU blit path);
				 * NULL for SNES/GBC. Applied per SOURCE pixel, before the nearest prescale. */
				if (cc_lut) v = cc_lut[(((v >> 12) & 0xFu) << 8) | (((v >> 7) & 0xFu) << 4) | ((v >> 1) & 0xFu)];
				unsigned int r = ((v >> 11) & 0x1fu) << 3, g = ((v >> 5) & 0x3fu) << 2, b = (v & 0x1fu) << 3;
				unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
				unsigned int *o = drow + x * ph;
				for (k = 0; k < ph; k++) o[k] = px;
				if ((filt >= 2 || (scan_cols && filt == 1)) && ph > 0u)   /* grid, or turned scanlines: dim last column of this run */
					o[ph - 1u] = 0xFF000000u | ((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
			}
			for (r_ = 1u; r_ < pv; r_++)
				memcpy(drow + r_ * fbpitch, drow, iw * 4u);   /* replicate the widened row */
			if ((filt >= 2 || (!scan_cols && filt == 1)) && pv > 0u) {   /* grid, or upright scanlines: dim the last of the pv rows */
				unsigned int *lr = drow + (pv - 1u) * fbpitch;
				for (x = 0; x < iw; x++) {
					unsigned int v = lr[x];
					lr[x] = 0xFF000000u | ((((v >> 16) & 0xffu) >> 1) << 16) |
					        ((((v >> 8) & 0xffu) >> 1) << 8) | ((v & 0xffu) >> 1);
				}
			}
		}
	}
	tp_pre = gpt4_get_current_tick();
#if AYANEO_ROT
	rot_hud_osd(vbuf, fbpitch, iw, ih);   /* FF/RW badge + volume bar, drawn upright on the turned game */
#else
	ayaneo_hud_draw(vbuf, fbpitch, 0, 0, (int)iw, (int)ih);   /* FF/RW speed badge (scaled with the game) */
#endif
	tp_hud = gpt4_get_current_tick();
	s_shot_pa = pbuf; s_shot_w = iw; s_shot_h = ih; s_shot_pitch = fbpitch;
	arch_clean_cache_range((unsigned int)vbuf, ih * fbpitch * 4u);
	tp_cln = gpt4_get_current_tick();

	/* Detect geometry changes (source res or output rect - dw/dh uniquely encode the aspect mode)
	 * - only then is the RSZ (a clock-only LK stub, so it persists across config_input) reprogrammed. */
	int geom_changed = (!s_rsz_setup || s_rsz_sw != sw || s_rsz_sh != sh ||
	                    s_rsz_dw != dw || s_rsz_dh != dh);

#if AYANEO_ROT
	/* Pace first, then align to the next frame end, THEN write the layer config: a config
	 * written at an arbitrary phase of the refresh is dropped by the overlay about half the
	 * time at a locked 2-refresh cadence (measured: 60 presents, 31 buffer switches per second),
	 * while one written right after frame end always lands (the pairmini's config_input did
	 * exactly that through its frame-done wait). */
	s_pace_expect = 0;   /* the game layer is checked elsewhere */
	{	/* the frame wait is not blit work: keep it out of g_rsz_show_us (the run-ahead budget) */
		unsigned int tw = gpt4_get_current_tick();
		rot_pace();   /* returns right after a frame end */
		tp_pace = gpt4_get_current_tick() - tw;
		g_dbg_pace_last_ticks = tp_pace;
		t0 += tp_pace;
	}
#endif
	/* Per-frame layer config: points OVL0_2L at the freshly written buffer, sized iw x ih,
	 * placed at (ox, oy). In video mode config_input waits frame-done then latches at the next
	 * vsync = paced + tear-free buffer handoff. */
	{
		disp_input_config in;
		memset(&in, 0, sizeof(in));
		in.layer = FB_LAYER; in.layer_en = 1;
		in.fmt = redoffset_32bit ? eBGRA8888 : eRGBA8888;
		in.addr = pbuf;
		in.src_x = 0; in.src_y = 0; in.src_w = iw; in.src_h = ih; in.src_pitch = fbpitch * 4u;
		in.dst_x = ox; in.dst_y = oy; in.dst_w = iw; in.dst_h = ih;
		in.aen = 1; in.alpha = 0xff;
#if AYANEO_ROT
		{
			extern int ayaneo_present_skip_framedone;
			int save = ayaneo_present_skip_framedone;
			ayaneo_present_skip_framedone = 1;
			primary_display_config_input(&in);
			ayaneo_present_skip_framedone = save;
		}
#else
		primary_display_config_input(&in);
#endif
		primary_display_trigger(1);
	}
	/* Post-trigger direct writes are live on OVL0_2L: pad the ROI (bars) and re-assert the
	 * layer offset (config_input's path resets the ROI to panel size each frame). */
	OVL0_2L_BGCLR = 0xFF000000u;               /* opaque black background = letterbox bars */
	OVL0_2L_ROI   = (hp << 16) | wp;           /* padded input ROI feeding RSZ */
	OVL0_2L_L0OFF = (oy << 16) | ox;           /* game position inside the padded ROI */
	if (geom_changed) {
		snes_rsz_program(wp, hp, W, H);    /* uniform scale of the padded frame to the panel */
		g_rszdbg[0]  = OVL0_2L_ROI;
		g_rszdbg[1]  = *(volatile unsigned int *)(0x14009038u);   /* L0_SRC_SIZE */
		g_rszdbg[2]  = OVL0_2L_L0OFF;
		g_rszdbg[3]  = *(volatile unsigned int *)(0x1400902Cu);   /* SRC_CON */
		g_rszdbg[4]  = RSZ0_R(0x000);
		g_rszdbg[5]  = RSZ0_R(0x004);
		g_rszdbg[6]  = RSZ0_R(0x010);
		g_rszdbg[7]  = RSZ0_R(0x014);
		g_rszdbg[8]  = (dw << 16) | dh;
		g_rszdbg[9]  = (xoff << 16) | yoff;
		g_rszdbg[10] = (iw << 16) | ih;
		g_rszdbg[11] = (wp << 16) | hp;
		s_rsz_setup = 1; s_rsz_sw = sw; s_rsz_sh = sh;
		s_rsz_dw = dw; s_rsz_dh = dh;
	}
	{
		unsigned int tend = gpt4_get_current_tick();
		g_dbg_show_parts[0] = p_od;
		g_dbg_show_parts[1] = (tp_rot - tp_od) / 13u;
		g_dbg_show_parts[2] = (tp_pre - tp_rot) / 13u;
		g_dbg_show_parts[3] = (tp_hud - tp_pre) / 13u;
		g_dbg_show_parts[4] = (tp_cln - tp_hud) / 13u;
		g_dbg_show_parts[5] = (tend - tp_cln - tp_pace) / 13u;   /* without the pace wait */
	}
	g_rsz_show_us = (gpt4_get_current_tick() - t0) / 13u;
#if AYANEO_ROT
	(void)wait_vsync;   /* paced + aligned above, before the layer config */
#else
	if (wait_vsync) priamry_display_wait_for_vsync();
#endif
	s_fb_flip ^= 1;
}

/* SNES RSZ wrapper: compute the target rect from the SNES aspect options, then present via the
 * shared hardware-RSZ path. */
static void ayaneo_snes_show_frame_rsz(const unsigned short *pix, unsigned int sw, unsigned int sh,
				       unsigned int spitch_px)
{
	unsigned int W = ayaneo_screen_w(), H = ayaneo_screen_h();
	extern volatile unsigned g_snes_aspect_x1000, g_snes_show_us;
	extern volatile int g_snes_stretch;
	extern int ayaneo_get_lcd_filter(void);
	extern int snes_benchmark_on(void);
	unsigned int sy_scale = (sh <= 240u) ? 4u : 2u, dw, dh;
	if (!pix || !fb_addr) return;
	if (g_snes_stretch) {
		dw = W; dh = H;
	} else if (AYANEO_ROT) {
		/* Konkr (960x640): the pairmini's 4x height (896 rows) does not fit, and clamping it
		 * made every aspect choice the same full screen rect. Fill the height and take the
		 * width from the core's aspect (4:3 -> 853, Pixel 8:7 -> 731, NTSC/PAL as reported). */
		unsigned int asp = g_snes_aspect_x1000;
		if (!asp) asp = (sw * 1000u) / (sh ? sh : 1u);
		dh = H; dw = (dh * asp) / 1000u;
		if (dw > W) { dw = W; dh = (W * 1000u) / asp; }
	} else {
		unsigned int asp = g_snes_aspect_x1000;
		dh = sh * sy_scale;
		dw = asp ? (dh * asp) / 1000u : sw * ((sw <= 256u) ? 4u : 2u);
	}
	if (dw > W) dw = W; if (dh > H) dh = H;
	if (dw < 1u) dw = 1u; if (dh < 1u) dh = 1u;
	ayaneo_rsz_present(pix, sw, sh, spitch_px, dw, dh, (W - dw) / 2u, (H - dh) / 2u,
			   ayaneo_get_lcd_filter_core(0), !snes_benchmark_on(), 0);
	g_snes_show_us = g_rsz_show_us;
}

/* SNES (snes9x) frame present. The core outputs RGB565 at 256xH (or 512xH hi-res); scale
 * to 1024 wide (256->4x, 512->2x) centred on the 1280x960 panel with black borders. Height
 * is 224/239 (progressive) or 448/478 (interlace) -> vertical scale chosen so 224/239 use
 * 4x/2x and the interlaced modes 2x/1x, all centred. Double-buffered like the GB path. */
void ayaneo_snes_show_frame(const unsigned short *pix, unsigned int sw, unsigned int sh, unsigned int spitch_px)
{
	{	/* The game always presents through its normal path, menu open or not - the Pico menu is now
		 * an independent hardware overlay (OVL0 L0, see ayaneo_menu_overlay), so it no longer forces
		 * the game onto the CPU path or pauses it. Aspect/filter changes preview live because the
		 * game keeps re-presenting underneath the static menu layer. */
		if (g_snes_rsz || AYANEO_ROT) {   /* the turned panel always presents through the resizer */
			ayaneo_snes_show_frame_rsz(pix, sw, sh, spitch_px);
			return;
		}
	}
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	/* Displayed rect. "Stretch" fills the whole panel (no bars): dw=W, dh=H, both fractional.
	 * Otherwise vertical is integer (4x/2x, so the LCD scanline filter has clean blocks) and
	 * width honours the core-reported aspect (g_snes_aspect_x1000 = dw/dh*1000); 0 -> integer. */
	extern volatile unsigned g_snes_aspect_x1000;
	extern volatile int g_snes_stretch;
	unsigned int sy_scale = (sh <= 240) ? 4 : 2;
	unsigned int dh, dw;
	if (g_snes_stretch) {
		dw = W; dh = H;
	} else {
		unsigned int asp = g_snes_aspect_x1000;
		dh = sh * sy_scale;
		dw = asp ? (dh * asp) / 1000u : sw * ((sw <= 256) ? 4u : 2u);
	}
	if (dw > W) dw = W;
	if (dh > H) dh = H;
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	int xoff = ((int)W - (int)dw) / 2, yoff = ((int)H - (int)dh) / 2;
	unsigned int *dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	unsigned int dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);
	unsigned int sy, sx, iy, cx;

	if (!fb_addr || !pix) return;
	if (xoff < 0) xoff = 0;
	if (yoff < 0) yoff = 0;
	extern unsigned int gpt4_get_current_tick(void);
	extern volatile unsigned g_snes_show_us;   /* blit+flush time (excl. vsync wait), for oem diag */
	unsigned int t_show0 = gpt4_get_current_tick();
	/* Borders (the letterbox outside the game) are static black, so clear BOTH buffers
	 * only when the source resolution OR the displayed rect (aspect switch) changes, not
	 * every frame. The per-frame game blit fully overwrites the game area. */
	{
		static unsigned int last_sw, last_sh, last_dw, last_dh;
		if (sw != last_sw || sh != last_sh || dw != last_dw || dh != last_dh) {
			memset(fb_addr, 0, fb_size);
			memset((unsigned char *)fb_addr + fb_size, 0, fb_size);
			arch_clean_cache_range((unsigned int)fb_addr, fb_size * 2);
			last_sw = sw; last_sh = sh; last_dw = dw; last_dh = dh;
		}
	}
	{
	/* Source-driven 2D scale: decode each source pixel's colour ONCE, then fill its run of
	 * destination columns AND rows via Bresenham spans (no per-dest-pixel RGB decode). Handles
	 * both integer-vertical (normal: qy = sy_scale, ry = 0) and fractional-vertical (Stretch:
	 * dh = panel height) uniformly. LCD filter (shared with GB/GBA): 1 scanlines (dim the last
	 * dest row of each source row), 2/3 grid (also dim the last dest column of each source
	 * pixel's run). Fast path when off. */
	int filt = ayaneo_get_lcd_filter_core(0);   /* SNES */
	unsigned int qx = dw / sw, rx = dw % sw;    /* dest cols per source pixel + remainder */
	unsigned int qy = dh / sh, ry = dh % sh;    /* dest rows per source pixel + remainder */
	unsigned int dy = (unsigned int)yoff, ey = 0;
	for (sy = 0; sy < sh; sy++) {
		const unsigned short *srow = pix + sy * spitch_px;
		unsigned int vspan = qy, dyend, dx = (unsigned int)xoff, ex_e = 0;
		ey += ry; if (ey >= sh) { ey -= sh; vspan++; }
		dyend = dy + vspan;
		if (!filt) {
			/* Fast path: render ONE destination row across all source pixels, then replicate
			 * it to the remaining (vspan-1) rows with memcpy. memcpy issues wide burst stores,
			 * so vertical scaling costs ~1 scalar row + N cheap copies instead of N scalar rows
			 * - a big win at high output heights (e.g. Stretch's 960 rows), which is why
			 * Stretch/large scales took a hard FPS hit before. */
			unsigned int *o0 = dst + dy * pitch_w;
			for (sx = 0; sx < sw; sx++) {
				unsigned int v = srow[sx];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
				unsigned int span = qx, ex;
				ex_e += rx; if (ex_e >= sw) { ex_e -= sw; span++; }
				ex = dx + span;
				for (cx = dx; cx < ex; cx++) o0[cx] = px;
				dx = ex;
			}
			for (iy = dy + 1; iy < dyend; iy++)
				memcpy(dst + iy * pitch_w + (unsigned int)xoff,
				       o0 + (unsigned int)xoff, (size_t)dw * 4u);
		} else for (sx = 0; sx < sw; sx++) {
			unsigned int v = srow[sx];
			unsigned int r = ((v >> 11) & 0x1f) << 3;
			unsigned int g = ((v >> 5) & 0x3f) << 2;
			unsigned int b = (v & 0x1f) << 3;
			unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
			unsigned int dk = 0xFF000000u | ((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
			unsigned int span = qx, ex;
			ex_e += rx; if (ex_e >= sw) { ex_e -= sw; span++; }
			ex = dx + span;
			/* Filter path, hoisted out of the inner column loop: the last dest row of each
			 * source row is dimmed (scanlines); grid (filt>=2) also dims the last dest column.
			 * Non-last rows are a tight px fill with at most one dk patch, no per-pixel branch. */
			{
				unsigned int lastr = dyend - 1;
				int grid = (filt >= 2);
				for (iy = dy; iy < dyend; iy++) {
					unsigned int *o = dst + iy * pitch_w;
					if (iy == lastr) {
						for (cx = dx; cx < ex; cx++) o[cx] = dk;   /* scanline row */
					} else {
						for (cx = dx; cx < ex; cx++) o[cx] = px;
						if (grid && ex > dx) o[ex - 1] = dk;       /* grid column */
					}
				}
			}
			dx = ex;
		}
		dy = dyend;
	}
	}
	/* The Pico menu is now an independent hardware overlay (ayaneo_menu_overlay), not drawn here. */
	ayaneo_draw_osd(dst, pitch_w, W, H);	/* transient volume/brightness slider (HW rocker) */
	ayaneo_hud_draw(dst, pitch_w, xoff, yoff, (int)dw, (int)dh);   /* FF/RW speed badge */
	extern volatile unsigned g_snes_flush_us;   /* isolated cache-clean cost, for oem diag */
	unsigned int t_flush0 = gpt4_get_current_tick();
	/* Flush ONLY the game rows [yoff, yoff+dh) - the letterbox is static (cleared once on geometry
	 * change above) and the OSD/HUD self-flush their own bands. Full-panel clean wasted ~0.33MB+ of
	 * cache traffic every frame (Stretch dh==H is unchanged). Mirrors Genesis/GBC/GBA. */
	arch_clean_cache_range((unsigned int)(dst + (unsigned int)yoff * pitch_w), (unsigned int)dh * pitch_w * 4);
	unsigned int t_flush1 = gpt4_get_current_tick();
	g_snes_flush_us = (t_flush1 - t_flush0) / 13u;               /* just the game-rows clean */
	g_snes_show_us  = (t_flush1 - t_show0) / 13u;                /* scale+osd+flush (no vsync) */
	ayaneo_present(dpa, W, H, pitch_w);
	/* Vsync-locked present: the SNES session runs the panel at ~60.11 Hz (vfp swap), so
	 * blocking one vsync here paces emulation to the real scan-out = smooth, tear-free,
	 * and it guarantees the presented buffer is scanned before the renderer reuses it.
	 * Benchmark (Uncap) mode skips the wait so the emulated FPS can exceed the panel rate. */
	{ extern int snes_benchmark_on(void); if (!snes_benchmark_on()) priamry_display_wait_for_vsync(); }
	s_fb_flip ^= 1;
}

/* Sega Genesis-Plus-GX present: variable native geometry (MD 320x224/256x224/320x240, SMS/SG
 * 256x192, GG cropped), RGB565 at a fixed 720px source stride. BASIC integer-scale CPU blit +
 * vsync-locked present (mirrors ayaneo_gb_show_frame); Fit/Stretch RSZ + LCD filter are added in
 * the parity phase. Centered, letterbox cleared once on a geometry change; FF/RW HUD drawn on top. */
/* Genesis display aspect (x1000) for the fit modes: 1 the core's own ratio (PAR corrected, ~4:3),
 * 3 exactly 4:3, 4 square pixels (the frame's own w:h), 5 16:9. 0 Pixel and 2 Stretch never ask. */
static unsigned gen_aspect_x1000(int mode, unsigned sw, unsigned sh)
{
	extern volatile unsigned g_genesis_aspect_x1000;
	switch (mode) {
	case 3:  return 1333u;
	case 4:  return sh ? sw * 1000u / sh : 1333u;
	case 5:  return 1778u;
	default: return g_genesis_aspect_x1000 ? g_genesis_aspect_x1000 : 1333u;
	}
}

void ayaneo_genesis_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px)
{
	extern unsigned int gpt4_get_current_tick(void);
	extern volatile int g_genesis_aspect;          /* 0 Pixel, 1 Core, 2 Stretch, 3 4:3, 4 Square, 5 16:9 (genesis_sd_run.c) */
	extern volatile unsigned g_genesis_aspect_x1000;
	extern volatile int g_genesis_filter;          /* 0=off 1=scanlines 2/3=grid */
	unsigned int t0 = gpt4_get_current_tick();
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int scale, s2, dw, dh, xoff, yoff, sx, sy, ix, iy;
	unsigned int *dst, dpa;
	int filt = g_genesis_filter;

	if (!fb_addr || !pix || !sw || !sh) return;

#if AYANEO_ROT
	{
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), dwr, dhr, a;
		if (g_genesis_aspect == 2) { dwr = LW; dhr = LH; }
		else if (g_genesis_aspect != 0) {
			a = gen_aspect_x1000(g_genesis_aspect, sw, sh);
			dhr = LH; dwr = LH * a / 1000u; if (dwr > LW) { dwr = LW; dhr = LW * 1000u / a; }
		} else { unsigned k = rot_int_scale(sw, sh); dwr = sw * k; dhr = sh * k; }
		ayaneo_rsz_present(pix, sw, sh, spitch_px, dwr, dhr, (LW - dwr) / 2u, (LH - dhr) / 2u, filt, 0, 0);
		g_dbg_blit_us = g_rsz_show_us;
		return;
	}
#endif
	/* Aspect modes: Fit / Stretch via the hardware RSZ; Pixel (0) uses the sharp integer CPU blit.
	 * Genesis is 4:3 on a 4:3 panel, so Fit uses the core's aspect (g_genesis_aspect_x1000, ~4:3)
	 * and Stretch fills the panel. RSZ stays on under the live overlay menu; Pixel/exit restores 1:1. */
	if (g_genesis_aspect != 0) {
		unsigned int dwr, dhr, a;
		if (g_genesis_aspect == 2) { dwr = W; dhr = H; }
		else { a = gen_aspect_x1000(g_genesis_aspect, sw, sh);
		       dhr = H; dwr = H * a / 1000u; if (dwr > W) { dwr = W; dhr = W * 1000u / a; } }
		ayaneo_rsz_present(pix, sw, sh, spitch_px, dwr, dhr, (W - dwr) / 2u, (H - dhr) / 2u, filt, 0, 0);
		g_dbg_blit_us = g_rsz_show_us;
		return;
	}
	if (s_rsz_setup) ayaneo_snes_rsz_restore();   /* Pixel / menu-open: back to the 1:1 path (once) */

	scale = W / sw; s2 = H / sh; if (s2 < scale) scale = s2; if (scale < 1) scale = 1;
	dw = sw * scale; dh = sh * scale;
	xoff = (W - dw) / 2; yoff = (H - dh) / 2;

	/* borders are static black - clear BOTH buffers only when the geometry changes */
	{
		static unsigned int last_sw, last_sh, last_sc;
		if (sw != last_sw || sh != last_sh || scale != last_sc) {
			memset(fb_addr, 0, fb_size);
			memset((unsigned char *)fb_addr + fb_size, 0, fb_size);
			arch_clean_cache_range((unsigned int)fb_addr, fb_size * 2);
			last_sw = sw; last_sh = sh; last_sc = scale;
		}
	}
	dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);

	if (!filt) {
		/* Fast path (no filter, the common case): render ONE destination row per source row
		 * (scalar), then replicate it to the remaining scale-1 rows with memcpy. memcpy issues wide
		 * vectorised stores, so vertical scaling costs ~1 scalar row + N cheap copies instead of N
		 * scalar rows - the fully-scalar scale*scale nested blit was the Pixel-aspect slowdown
		 * (the RSZ Fit/Stretch path already replicated rows by memcpy, so it ran fine). */
		for (sy = 0; sy < sh; sy++) {
			const unsigned short *srow = pix + sy * spitch_px;
			unsigned int *o0 = dst + (unsigned int)(yoff + (int)(sy * scale)) * pitch_w + (unsigned int)xoff;
			unsigned int col = 0;
			for (sx = 0; sx < sw; sx++) {
				unsigned int v = srow[sx];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
				for (ix = 0; ix < scale; ix++) o0[col++] = px;
			}
			for (iy = 1; iy < scale; iy++)
				memcpy(o0 + iy * pitch_w, o0, (size_t)dw * 4u);
		}
	} else {
		for (sy = 0; sy < sh; sy++) {
			const unsigned short *srow = pix + sy * spitch_px;
			for (sx = 0; sx < sw; sx++) {
				unsigned int v = srow[sx];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
				unsigned int dk = 0xFF000000u | ((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
				for (iy = 0; iy < scale; iy++) {
					unsigned int *o = dst + (yoff + sy * scale + iy) * pitch_w + (xoff + sx * scale);
					int lastrow = (iy == scale - 1);
					for (ix = 0; ix < scale; ix++) {
						unsigned int cc = px; int lastcol = (ix == scale - 1);
						if (filt == 1 && lastrow) cc = dk;
						else if (filt >= 2 && (lastrow || lastcol)) cc = dk;
						o[ix] = cc;
					}
				}
			}
		}
	}
	ayaneo_draw_osd(dst, pitch_w, W, H);
	ayaneo_hud_draw(dst, pitch_w, (int)xoff, (int)yoff, (int)dw, (int)dh);   /* FF/RW speed badge */
	/* Flush ONLY the game rows (the letterbox is static black, flushed once on geometry change; the
	 * OSD/HUD self-flush their own regions) - mirrors the GBC path. The old full-panel clean touched
	 * ~60% more cache lines every frame for no benefit. */
	arch_clean_cache_range((unsigned int)(dst + (unsigned int)yoff * pitch_w), (unsigned int)dh * pitch_w * 4);
	g_dbg_blit_us = (gpt4_get_current_tick() - t0) / 13u;
	/* Present. primary_display_config_input (inside ayaneo_present) already WAITS for FRAME_DONE in
	 * DSI video mode, which paces the emulation to the panel's 60 Hz - exactly like the GBC path and
	 * the RSZ (Fit/Stretch) path (which passes wait_vsync=0). The old explicit wait here was a SECOND
	 * blocking wait per frame = two vsync periods per present = a hard 30fps cap in Pixel mode, while
	 * Fit/Stretch ran at 60. That double-wait was the Pixel-aspect slowdown. */
	ayaneo_present(dpa, W, H, pitch_w);
	s_fb_flip ^= 1;
}

/* Blank BOTH game frame buffers to black. Called at the menu -> game transition
 * so no menu / BIOS-intro pixels linger in any area the game frame does not draw
 * (belt-and-suspenders now that the 6x game fills the panel). */
void ayaneo_gbc_blank(void)
{
	if (!fb_addr)
		return;
	memset(fb_addr, 0, fb_size);
	memset((unsigned char *)fb_addr + fb_size, 0, fb_size);
	arch_clean_cache_range((unsigned int)fb_addr, fb_size * 2);
}

/* Clear ONLY the letterbox (the region outside the centred game) in BOTH buffers,
 * leaving the game area untouched. Used at the launch-punch END: the punch left the
 * menu snapshot in the letterbox, but the game area is already full gameplay (radius
 * covers it). A full ayaneo_gbc_blank() there memsets the LIVE buffer black =
 * a one-frame black flash = the menu->game transition flicker. Clearing only the
 * letterbox never blacks the live game centre, so there is no flash. */
void ayaneo_gbc_clear_letterbox(void)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	int dw = GBC_SRC_W * GBC_SCALE, dh = GBC_SRC_H * GBC_SCALE;
	int xoff = ((int)W - dw) / 2, yoff = ((int)H - dh) / 2;
	int b, y;

	if (!fb_addr)
		return;
#if AYANEO_ROT
	/* The game is presented by the resizer path with its own black bars; the CPU buffers only
	 * ever hold turned menu canvases here, so there is no live game centre to protect. */
	(void)pitch; (void)dw; (void)dh; (void)xoff; (void)yoff; (void)b; (void)y;
	ayaneo_gbc_blank();
	return;
#endif
	for (b = 0; b < 2; b++) {
		unsigned int *fb = (unsigned int *)((unsigned char *)fb_addr + (b ? fb_size : 0));
		for (y = 0; y < yoff; y++)                 memset(fb + (unsigned)y * pitch, 0, W * 4);
		for (y = yoff + dh; y < (int)H; y++)       memset(fb + (unsigned)y * pitch, 0, W * 4);
		for (y = yoff; y < yoff + dh; y++) {       /* left + right side strips */
			memset(fb + (unsigned)y * pitch, 0, xoff * 4);
			memset(fb + (unsigned)y * pitch + (xoff + dw), 0, (W - xoff - dw) * 4);
		}
	}
	arch_clean_cache_range((unsigned int)fb_addr, fb_size * 2);
}

/* Punch-hole launch transition: composite the LIVE game frame (`pix`, the same
 * 240x160 -> GBC_SCALE centred render as ayaneo_gbc_show_frame, black letterbox)
 * INSIDE a circle of `radius` centred at (cx,cy), and the frozen menu `snap` (a
 * full fb-size BGRA snapshot the menu captured on launch) OUTSIDE it. Growing
 * radius each call reveals real gameplay while the menu is eaten by the hole. The
 * pixel math is in gba_punch_composite (emu/gba/menu/gba_punch.h) so it is
 * host-testable; this wrapper only picks the back buffer, flushes and presents. */
void ayaneo_gba_punch_frame(const unsigned short *pix, const unsigned int *snap,
			    int cx, int cy, int radius)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	int dw = GBC_SRC_W * GBC_SCALE, dh = GBC_SRC_H * GBC_SCALE;
	int xoff = ((int)W - dw) / 2, yoff = ((int)H - dh) / 2;
	unsigned int *dst;
	unsigned int dpa;

	if (!fb_addr || !snap)
		return;
#if AYANEO_ROT
	{
		/* Composite on the virtual canvas at the canvas' own integer scale (5x = 1200x800 on
		 * 1280x853, a few percent short of the full-fill the game then runs at). */
		unsigned CW, CH, k;
		ayaneo_canvas_dims(&CW, &CH);
		k = CW / GBC_SRC_W; if (CH / GBC_SRC_H < k) k = CH / GBC_SRC_H; if (!k) k = 1;
		(void)dpa; (void)dw; (void)dh; (void)xoff; (void)yoff;
		dst = (unsigned int *)ROT_CANVAS_PA;
		gba_punch_composite(dst, snap, (int)CW, (int)CW, (int)CH, pix, cx, cy, radius,
				    (int)k, GBC_SRC_W, GBC_SRC_H,
				    ((int)CW - GBC_SRC_W * (int)k) / 2, ((int)CH - GBC_SRC_H * (int)k) / 2);
		ayaneo_canvas_present();   /* scales, turns, presents, waits one vsync, flips */
		return;
	}
#endif
	dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);
	gba_punch_composite(dst, snap, (int)pitch_w, (int)W, (int)H, pix,
			    cx, cy, radius, GBC_SCALE, GBC_SRC_W, GBC_SRC_H, xoff, yoff);
	arch_clean_cache_range((unsigned int)dst, H * pitch_w * 4);
	ayaneo_present(dpa, W, H, pitch_w);
	/* Block one vsync before flipping so the swap is live before the next frame is
	 * composited into the other buffer (same fix as ayaneo_canvas_present). The
	 * launch punch composites the full-frame menu snapshot, so without this the
	 * partial-black flip glitch shows = the menu->game transition flicker. */
	priamry_display_wait_for_vsync();
	s_fb_flip ^= 1;
}

/* FAST launch punch (same idea as the smooth reverse): freeze the current game frame
 * into a full-screen BGRA buffer ONCE, then each growing-circle frame is memcpy-only
 * (gba_punch_composite_pre) = ~5ms not ~50ms, so the driver can frame-pace a smooth
 * 60fps opening. The game is paused for the ~0.3s punch (imperceptible at launch). */
#define GBA_PUNCH_GAME_FULL_PA 0x55900000u

/* Fill the full-screen BGRA punch buffer with the game frame nearest-scaled into the rect (dw x dh)
 * centred on the panel, opaque-black bars elsewhere. This is the fractional (both-axes) form of the
 * SNES punch prerender, used for the GBA/GBC Fit/Stretch aspect modes so the launch circle opens onto
 * the SAME geometry the RSZ show_frame path will present (no first-frame geometry pop). Bounded: the
 * source index is (x-xoff)*sw/dw < sw and (y-yoff)*sh/dh < sh, so it never reads outside the frame. */
static void punch_fill_scaled(unsigned int *gf, unsigned int pitch, unsigned int W, unsigned int H,
			      const unsigned short *pix, unsigned int sw, unsigned int sh, unsigned int spitch,
			      unsigned int dw, unsigned int dh)
{
	int xoff, yoff; unsigned int xstep, ystep, y, x;
	if (dw > W) dw = W; if (dh > H) dh = H;
	if (dw < 1u) dw = 1u; if (dh < 1u) dh = 1u;
	xoff = ((int)W - (int)dw) / 2; yoff = ((int)H - (int)dh) / 2;
	if (xoff < 0) xoff = 0; if (yoff < 0) yoff = 0;
	xstep = (sw << 16) / dw;
	ystep = (sh << 16) / dh;
	for (y = 0; y < H; y++) {
		unsigned int *orow = gf + y * pitch;
		int in_gy = ((int)y >= yoff && y < (unsigned)yoff + dh);
		const unsigned short *srow = in_gy ? (pix + (((y - (unsigned)yoff) * ystep) >> 16) * spitch) : 0;
		unsigned int acc = 0;
		for (x = 0; x < W; x++) {
			if (in_gy && (int)x >= xoff && x < (unsigned)xoff + dw) {
				unsigned int v = srow[acc >> 16]; acc += xstep;
				unsigned int r = ((v >> 11) & 0x1fu) << 3, g = ((v >> 5) & 0x3fu) << 2, b = (v & 0x1fu) << 3;
				orow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
			} else orow[x] = 0xFF000000u;
		}
	}
}

#if AYANEO_ROT
/* Bake a native frame into the punch buffer in VIRTUAL CANVAS space with the same rect the
 * resizer path shows it at: aspect 0 = largest integer fit of the logical screen, 1 = fit,
 * 2 = stretch, computed on the logical 960x640 then scaled to the canvas (4/3). The fractional
 * fill is fine: the canvas is scaled 3/4 on present, so this lands on the panel at exactly the
 * live geometry (no geometry pop when the punch finishes and the game takes over). */
static void rot_prerender_canvas(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch,
				 int aspect)
{
	unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), CW, CH, dwr, dhr;
	ayaneo_canvas_dims(&CW, &CH);
	if (aspect == 2) { dwr = LW; dhr = LH; }
	else if (aspect == 1) {
		if (LW * sh <= LH * sw) { dwr = LW; dhr = LW * sh / sw; }
		else { dhr = LH; dwr = LH * sw / sh; }
	} else { unsigned k = rot_int_scale(sw, sh); dwr = sw * k; dhr = sh * k; }
	punch_fill_scaled((unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, CW, CW, CH,
			  pix, sw, sh, spitch, (dwr * CW + LW / 2u) / LW, (dhr * CH + LH / 2u) / LH);
	arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, CW * CH * 4u);
}
#endif

void ayaneo_gba_punch_prerender(const unsigned short *pix)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	extern volatile int g_gba_aspect;
	if (!pix) return;
#if AYANEO_ROT
	/* The punch buffer is composited onto the virtual canvas, so bake the game there with the
	 * rect the live resizer path will show, mapped from the logical screen to canvas scale. */
	rot_prerender_canvas(pix, GBC_SRC_W, GBC_SRC_H, GBC_SRC_W, g_gba_aspect);
	return;
#endif
	if (g_gba_aspect != 0) {   /* Fit/Stretch: match the RSZ show_frame rect (no launch pop) */
		unsigned int dwr, dhr;
		if (g_gba_aspect == 2) { dwr = W; dhr = H; }
		else if (W * (unsigned)GBC_SRC_H <= H * (unsigned)GBC_SRC_W) { dwr = W; dhr = W * GBC_SRC_H / GBC_SRC_W; }
		else { dhr = H; dwr = H * GBC_SRC_W / GBC_SRC_H; }
		punch_fill_scaled((unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, pitch, W, H,
				  pix, GBC_SRC_W, GBC_SRC_H, GBC_SRC_W, dwr, dhr);
	} else {                   /* Pixel Perfect: crisp integer scale (unchanged) */
		int dw = GBC_SRC_W * GBC_SCALE, dh = GBC_SRC_H * GBC_SCALE;
		gba_punch_prerender((uint32_t *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, (int)pitch,
				    (int)W, (int)H, pix, GBC_SCALE, GBC_SRC_W, GBC_SRC_H,
				    ((int)W - dw) / 2, ((int)H - dh) / 2);
	}
	arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, pitch * H * 4);
}

/* Same prerender for a real GB/GBC frame (160x144 integer 6x), so the launch punch
 * opens onto the correct GB geometry (ayaneo_gba_punch_prerender bakes the GBA
 * 240x160x5 geometry in this build). Composite step reuses ayaneo_gba_punch_frame_pre. */
void ayaneo_gb_punch_prerender(const unsigned short *pix)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	const int SW = 160, SH = 144, SC = 6;
	extern volatile int g_gbc_aspect;
	if (!pix) return;
#if AYANEO_ROT
	rot_prerender_canvas(pix, SW, SH, SW, g_gbc_aspect);
	return;
#endif
	if (g_gbc_aspect != 0) {   /* Fit/Stretch: match the RSZ show_frame rect (no launch pop) */
		unsigned int dwr, dhr;
		if (g_gbc_aspect == 2) { dwr = W; dhr = H; }
		else if (W * (unsigned)SH <= H * (unsigned)SW) { dwr = W; dhr = W * SH / SW; }
		else { dhr = H; dwr = H * SW / SH; }
		punch_fill_scaled((unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, pitch, W, H,
				  pix, SW, SH, SW, dwr, dhr);
	} else {                   /* Pixel Perfect: crisp integer 6x (unchanged) */
		int dw = SW * SC, dh = SH * SC;
		gba_punch_prerender((uint32_t *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, (int)pitch,
				    (int)W, (int)H, pix, SC, SW, SH,
				    ((int)W - dw) / 2, ((int)H - dh) / 2);
	}
	arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, pitch * H * 4);
}

/* SNES variant: pre-convert a frozen SNES frame (256/512 wide, RGB565, stride spitch_px)
 * to the full-screen BGRA game buffer the punch compositor reads, using the SAME aspect-
 * aware scale as ayaneo_snes_show_frame so the launch circle opens onto matching geometry. */
void ayaneo_snes_punch_prerender(const unsigned short *pix, unsigned int sw, unsigned int sh,
				 unsigned int spitch_px)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int *gf = (unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA;
	unsigned int sy_scale = (sh <= 240) ? 4 : 2;
	unsigned int dh = sh * sy_scale;
	extern volatile unsigned g_snes_aspect_x1000;
	unsigned int asp = g_snes_aspect_x1000;
	unsigned int dw = asp ? (dh * asp) / 1000u : sw * ((sw <= 256) ? 4u : 2u);
	int xoff, yoff; unsigned int xstep, y, x;
	if (!pix) return;
#if AYANEO_ROT
	{
		/* Same rect as ayaneo_snes_show_frame_rsz on the logical screen, then canvas scaled. */
		extern volatile int g_snes_stretch;
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), CW, CH, dwr, dhr;
		(void)pitch; (void)gf; (void)xoff; (void)yoff; (void)xstep; (void)y; (void)x;
		ayaneo_canvas_dims(&CW, &CH);
		if (g_snes_stretch) { dwr = LW; dhr = LH; }
		else { dhr = dh; dwr = dw; }
		if (dwr > LW) dwr = LW; if (dhr > LH) dhr = LH;
		if (dwr < 1u) dwr = 1u; if (dhr < 1u) dhr = 1u;
		punch_fill_scaled((unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, CW, CW, CH,
				  pix, sw, sh, spitch_px, (dwr * CW + LW / 2u) / LW, (dhr * CH + LH / 2u) / LH);
		arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, CW * CH * 4u);
		return;
	}
#endif
	if (dw > W) dw = W; if (dw < 1) dw = 1;
	xoff = ((int)W - (int)dw) / 2; yoff = ((int)H - (int)dh) / 2;
	if (xoff < 0) xoff = 0; if (yoff < 0) yoff = 0;
	xstep = (sw << 16) / dw;
	for (y = 0; y < H; y++) {
		unsigned int *orow = gf + y * pitch;
		int in_gy = ((int)y >= yoff && y < (unsigned)yoff + dh);
		const unsigned short *srow = in_gy ? (pix + ((y - (unsigned)yoff) / sy_scale) * spitch_px) : 0;
		unsigned int acc = 0;
		for (x = 0; x < W; x++) {
			if (in_gy && (int)x >= xoff && x < (unsigned)xoff + dw) {
				unsigned int v = srow[acc >> 16]; acc += xstep;
				unsigned int r = ((v >> 11) & 0x1f) << 3, g = ((v >> 5) & 0x3f) << 2, b = (v & 0x1f) << 3;
				orow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
			} else orow[x] = 0xFF000000u;
		}
	}
	arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, pitch * H * 4);
}

/* Genesis variant: pre-convert a frozen Genesis frame (RGB565, stride spitch_px) to the
 * full-screen BGRA game buffer the punch compositor reads, using the SAME aspect-mode geometry
 * as ayaneo_genesis_show_frame (0=Pixel integer, 1=Fit core-aspect, 2=Stretch full-panel) so the
 * launch circle opens - and the exit circle shrinks - onto the exact live on-screen geometry. */
void ayaneo_genesis_punch_prerender(const unsigned short *pix, unsigned int sw, unsigned int sh,
				    unsigned int spitch_px)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int *gf = (unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA;
	extern volatile unsigned g_genesis_aspect_x1000;
	extern volatile int g_genesis_aspect;
	unsigned int dw, dh, xstep, ystep, x, y;
	int xoff, yoff;
	if (!pix || !sw || !sh) return;
#if AYANEO_ROT
	{
		unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h(), CW, CH, dwr, dhr, a;
		(void)pitch; (void)gf; (void)xoff; (void)yoff; (void)xstep; (void)ystep; (void)x; (void)y;
		ayaneo_canvas_dims(&CW, &CH);
		if (g_genesis_aspect == 2) { dwr = LW; dhr = LH; }
		else if (g_genesis_aspect != 0) {
			a = gen_aspect_x1000(g_genesis_aspect, sw, sh);
			dhr = LH; dwr = LH * a / 1000u; if (dwr > LW) { dwr = LW; dhr = LW * 1000u / a; }
		} else { unsigned k = rot_int_scale(sw, sh); dwr = sw * k; dhr = sh * k; }
		punch_fill_scaled((unsigned int *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA, CW, CW, CH,
				  pix, sw, sh, spitch_px, (dwr * CW + LW / 2u) / LW, (dhr * CH + LH / 2u) / LH);
		arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, CW * CH * 4u);
		return;
	}
#endif
	if (g_genesis_aspect == 2) { dw = W; dh = H; }
	else if (g_genesis_aspect != 0) {
		unsigned int a = gen_aspect_x1000(g_genesis_aspect, sw, sh);
		dh = H; dw = H * a / 1000u; if (dw > W) { dw = W; dh = W * 1000u / a; }
	} else {                                     /* Pixel: integer scale = min(W/sw, H/sh) */
		unsigned int s = W / sw, s2 = H / sh; if (s2 < s) s = s2; if (s < 1) s = 1;
		dw = sw * s; dh = sh * s;
	}
	if (dw > W) dw = W; if (dh > H) dh = H; if (dw < 1) dw = 1; if (dh < 1) dh = 1;
	xoff = ((int)W - (int)dw) / 2; yoff = ((int)H - (int)dh) / 2;
	if (xoff < 0) xoff = 0; if (yoff < 0) yoff = 0;
	xstep = (sw << 16) / dw; ystep = (sh << 16) / dh;
	for (y = 0; y < H; y++) {
		unsigned int *orow = gf + y * pitch;
		int in_gy = ((int)y >= yoff && y < (unsigned)yoff + dh);
		const unsigned short *srow = in_gy ? (pix + (((y - (unsigned)yoff) * ystep) >> 16) * spitch_px) : 0;
		unsigned int acc = 0;
		for (x = 0; x < W; x++) {
			if (in_gy && (int)x >= xoff && x < (unsigned)xoff + dw) {
				unsigned int v = srow[acc >> 16]; acc += xstep;
				unsigned int r = ((v >> 11) & 0x1f) << 3, g = ((v >> 5) & 0x3f) << 2, b = (v & 0x1f) << 3;
				orow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
			} else orow[x] = 0xFF000000u;
		}
	}
	arch_clean_cache_range(GBA_PUNCH_GAME_FULL_PA, pitch * H * 4);
}

void ayaneo_gba_punch_frame_pre(const unsigned int *snap, int radius)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int *dst;
	unsigned int dpa;
	if (!fb_addr || !snap) return;
#if AYANEO_ROT
	{
		unsigned CW, CH;
		ayaneo_canvas_dims(&CW, &CH);
		(void)dpa;
		dst = (unsigned int *)ROT_CANVAS_PA;
		gba_punch_composite_pre(dst, snap, (const uint32_t *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA,
					(int)CW, (int)CW, (int)CH, -1, -1, radius);
		ayaneo_canvas_present();
		return;
	}
#endif
	dst = (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
	dpa = (unsigned int)fb_addr_pa + (s_fb_flip ? fb_size : 0);
	gba_punch_composite_pre(dst, snap, (const uint32_t *)(uintptr_t)GBA_PUNCH_GAME_FULL_PA,
				(int)pitch, (int)W, (int)H, -1, -1, radius);
	arch_clean_cache_range((unsigned int)dst, H * pitch * 4);
	ayaneo_present(dpa, W, H, pitch);
	priamry_display_wait_for_vsync();
	s_fb_flip ^= 1;
}
#endif /* AYANEO_GBC */

/*
 * Boot animation player. Reads a compressed frame blob from the "logo"
 * partition (header: 'GBA1', ver, w, h, nframes, fps; then per-frame
 * [u32 raw-deflate-len][data] of a w*h RGB565 image), and plays it once at
 * AYANEO_ANIM_FPS: inflate -> RGB565 -> BGRA8888 nearest-2x upscale ->
 * letterboxed into the framebuffer -> present. The blob ends with fade-to-black
 * frames, so when playback finishes the panel is already black; the thread then
 * holds until boot stops it at the kernel handoff. All buffers live in the
 * display VRAM (pages 0/1 are the two scan-out buffers, page 2 is scratch), so
 * no extra reserved memory is taken.
 */
/*
 * Streaming reader over the animation partition. The blob (up to ~20 MB at
 * 1280x720/60fps) is far larger than the VRAM scratch, so we keep a sliding
 * window buffer, refilling from the partition when the requested run isn't fully
 * resident. Returns 1 if at least 'need' bytes are available at sbuf+*spos.
 */
static int anim_ensure(const char *part, unsigned char *sbuf, unsigned int scap,
		       unsigned int *poff, unsigned int *svalid, unsigned int *spos,
		       unsigned int need)
{
	if (*svalid - *spos >= need)
		return 1;
	if (*spos) {			/* compact remaining bytes to the front */
		unsigned int avail = *svalid - *spos;

		memmove(sbuf, sbuf + *spos, avail);
		*svalid = avail;
		*spos = 0;
	}
	while (*svalid - *spos < need && *svalid < scap) {
		int got = (int)partition_read(part, *poff, sbuf + *svalid,
					      scap - *svalid);

		if (got <= 0)
			break;
		*poff += (unsigned int)got;
		*svalid += (unsigned int)got;
	}
	return (*svalid - *spos >= need);
}

/*
 * RGB565 (sw x sh) -> BGRA8888, nearest-neighbour scale into the letterbox rect.
 * 'scale' (0..256) dims every channel, used by the code-driven fade-out.
 */
static void anim_blit(const unsigned char *rgb, unsigned int *dst,
		      unsigned int sw, unsigned int sh, unsigned int dw, unsigned int dh,
		      unsigned int xoff, unsigned int yoff, unsigned int pitch_w,
		      unsigned int scale, unsigned int crop_y, unsigned int scaled_h)
{
	unsigned int ox, oy;

	for (oy = 0; oy < dh; oy++) {
		const unsigned short *srow = (const unsigned short *)
			(rgb + ((oy + crop_y) * sh / scaled_h) * sw * 2);
		unsigned int *orow = dst + (yoff + oy) * pitch_w + xoff;

		for (ox = 0; ox < dw; ox++) {
			unsigned int v = srow[s_sxmap[ox]];
			unsigned int r = ((v >> 11) & 0x1f) << 3;
			unsigned int g = ((v >> 5) & 0x3f) << 2;
			unsigned int b = (v & 0x1f) << 3;

			if (scale < 256) {
				r = (r * scale) >> 8;
				g = (g * scale) >> 8;
				b = (b * scale) >> 8;
			}
			orow[ox] = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
	}
	arch_clean_cache_range((unsigned int)(dst + yoff * pitch_w), dh * pitch_w * 4);
}

static int ayaneo_rainbow_thread(void *arg)
{
	unsigned int W, H, pitch_w;
	unsigned int disp_pa[2];
	unsigned int *disp_va[2];
	unsigned char *rgb, *sbuf;
	unsigned int scap, poff, svalid, spos;
	unsigned int magic, sw, sh, nf, fps, i, dw, dh, xoff, yoff, start_ms, base;
	unsigned int scaled_w, scaled_h, crop_x, crop_y;

	W = CFG_DISPLAY_WIDTH;
	H = CFG_DISPLAY_HEIGHT;
	pitch_w = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT);

	disp_va[0] = (unsigned int *)fb_addr;
	disp_va[1] = (unsigned int *)((unsigned char *)fb_addr + fb_size);
	disp_pa[0] = (unsigned int)fb_addr_pa;
	disp_pa[1] = (unsigned int)fb_addr_pa + fb_size;
	/* VRAM page 2: decoded RGB565 frame + the streaming window buffer */
	rgb  = (unsigned char *)fb_addr + 2 * fb_size;
	sbuf = rgb + AYANEO_ANIM_RGBMAX;
	scap = fb_size - AYANEO_ANIM_RGBMAX - 4096;

	/* only our FB_LAYER should show */
	{
		disp_input_config din;

		memset(&din, 0, sizeof(din));
		din.layer = BOOT_MENU_LAYER;
		din.layer_en = 0;
		primary_display_config_input(&din);
	}

	/* prime the window and detect raw (offset 0) vs MTK-header-wrapped (512) */
	poff = 0; svalid = 0; spos = 0;
	anim_ensure(AYANEO_ANIM_PART, sbuf, scap, &poff, &svalid, &spos,
		    AYANEO_ANIM_HDR + 16);
	base = 0;
	if (rd32(sbuf + 0) != AYANEO_ANIM_MAGIC && svalid >= AYANEO_ANIM_HDR + 16 &&
	    rd32(sbuf + AYANEO_ANIM_HDR) == AYANEO_ANIM_MAGIC)
		base = AYANEO_ANIM_HDR;

	magic = rd32(sbuf + base);
	sw = rd16(sbuf + base + 8);
	sh = rd16(sbuf + base + 10);
	nf = rd16(sbuf + base + 12);
	fps = rd16(sbuf + base + 14);
	if (magic != AYANEO_ANIM_MAGIC || sw == 0 || sh == 0 || sw > 1280 ||
	    sh > H || nf == 0 || sw * sh * 2 > AYANEO_ANIM_RGBMAX) {
#ifdef AYANEO_DEBUG_LOGGING
		dprintf(CRITICAL, "AYANEO_ANIM: bad blob magic=0x%x %ux%u n=%u\n",
			magic, sw, sh, nf);
#endif
		s_rainbow_exited = 1;
		return 0;
	}
	if (fps == 0 || fps > 120)
		fps = 30;
	spos = base + 16;		/* consume the header */

	/*
	 * "Cover" scaling: scale the frame keeping aspect so it fills the whole
	 * panel, then crop the overflowing dimension (this 16:9 clip on a 4:3
	 * panel scales to fill the height and crops the left/right sides). The
	 * output is the full panel; crop_x/crop_y index into the scaled image.
	 */
	scaled_w = W;
	scaled_h = W * sh / sw;
	if (scaled_h < H) {			/* not tall enough -> scale to height */
		scaled_h = H;
		scaled_w = H * sw / sh;
	}
	dw = W; dh = H; xoff = 0; yoff = 0;
	crop_x = (scaled_w - W) / 2;
	crop_y = (scaled_h - H) / 2;
	{
		unsigned int ox;
		for (ox = 0; ox < dw; ox++)
			s_sxmap[ox] = (unsigned short)((ox + crop_x) * sw / scaled_w);
	}

	/* black both buffers once for the letterbox bars */
	memset(disp_va[0], 0, fb_size);
	memset(disp_va[1], 0, fb_size);
	arch_clean_cache_range((unsigned int)disp_va[0], fb_size);
	arch_clean_cache_range((unsigned int)disp_va[1], fb_size);

#ifdef AYANEO_DEBUG_LOGGING
	dprintf(CRITICAL, "AYANEO_ANIM: %ux%u n=%u fps=%u dw=%u dh=%u scap=%u base=%u\n",
		sw, sh, nf, fps, dw, dh, scap, base);
#endif

	s_rainbow_start_ms = start_ms = (unsigned int)current_time();

#ifdef AYANEO_BOOT_AUDIO
	/* fire the boot sound once, in sync with the first animation frame */
	ayaneo_boot_audio_start();
#endif

	/*
	 * Frame-skip pacing: pick the frame that should be showing for the elapsed
	 * time and only decode/blit that one, streaming past (no decode) any frames
	 * we are too slow to display. This keeps the animation at correct wall-clock
	 * speed (60fps timeline) instead of playing every frame in slow motion when
	 * the single core can't decode fast enough.
	 */
	i = 0;			/* frames consumed from the stream */
	{
		unsigned int disp_i = 0, clen;
		unsigned long zlen;
		int shown = 0, have_frame = 0;

		/*
		 * Always play the whole animation through (real-time frame-skip), even
		 * if boot is already ready (s_fade_request) - the kernel handoff waits
		 * for the animation to finish and only then fades. The fade is done in
		 * code below, not baked into the blob. The thread stays at HIGH_PRIORITY
		 * for the entire animation so every frame presents crisply.
		 */
		while (!s_rainbow_stop) {
			unsigned int want = (unsigned int)((unsigned long)
				(current_time() - start_ms) * fps / 1000u);

			if (want >= nf)
				break;
			if (want < i) {
				thread_sleep(2);
				continue;
			}
			while (i <= want && !s_rainbow_stop) {
				int show = (i == want);

				if (!anim_ensure(AYANEO_ANIM_PART, sbuf, scap,
						 &poff, &svalid, &spos, 4))
					goto anim_done;
				clen = rd32(sbuf + spos);
				spos += 4;
				if (clen == 0 || clen > scap - 4)
					goto anim_done;
				if (!anim_ensure(AYANEO_ANIM_PART, sbuf, scap,
						 &poff, &svalid, &spos, clen))
					goto anim_done;
				if (show) {
					zlen = clen;
					if (zunzip(sbuf + spos, &zlen, rgb,
						   (int)(sw * sh * 2), 0) != 0)
						goto anim_done;
				}
				spos += clen;
				i++;
				if (!show)
					continue;
#if AYANEO_ROT
				/* Cover the logical screen: scale to its height and crop the width
				 * symmetrically at the source, then let the resizer path turn + scale. */
				{
					unsigned LW = ayaneo_screen_w(), LH = ayaneo_screen_h();
					unsigned csw = (sh ? LW * sh / LH : sw), csx;
					if (csw > sw) csw = sw;
					csx = (sw - csw) / 2u;
					ayaneo_rsz_present((const unsigned short *)rgb + csx, csw, sh, sw,
							   LW, LH, 0, 0, 0, 0, 0);
				}
#else
				anim_blit(rgb, disp_va[disp_i & 1], sw, sh, dw, dh,
					  xoff, yoff, pitch_w, 256, crop_y, scaled_h);
				ayaneo_present(disp_pa[disp_i & 1], W, H, pitch_w);
#endif
				disp_i++;
				have_frame = 1;
				if (!shown) { mt65xx_backlight_on(); ayaneo_apply_persisted_brightness(); shown = 1; }
			}
		}

		/*
		 * The full animation has played, including the fade-out to white or
		 * black which is baked into the blob's tail frames. The last frame is
		 * already the solid fade colour, so just hold it until the kernel
		 * takes over the display.
		 */
		(void)have_frame;
	}
anim_done:

	s_anim_complete = 1;

#ifdef AYANEO_DEBUG_LOGGING
	dprintf(CRITICAL, "AYANEO_ANIM: consumed %u/%u frames, holding\n", i, nf);
#endif
	/* animation ends on black (baked fade); hold until boot stops us */
	while (!s_rainbow_stop)
		thread_sleep(20);

	s_rainbow_exited = 1;
	return 0;
}

#if defined(AYANEO_GBC) || defined(AYANEO_GBA)
extern int ayaneo_gbc_select_held(void);
extern void ayaneo_settings_load(void);
extern int ayaneo_get_skip_boot(void);
#endif

void video_rainbow_boot_start(void)
{
	thread_t *t;

	s_rainbow_stop = 0;
	s_rainbow_exited = 0;
	s_anim_complete = 0;
	s_fade_request = 0;

#if defined(AYANEO_GBC) || defined(AYANEO_GBA)
	/* Skip the animation + chime and hand off immediately when: Select is held,
	 * the persisted "skip boot" setting is on, or this is a charger-insert
	 * power-on (offline charging) - in that last case the emulator hook shows
	 * the charging screen instead of booting the game. */
	ayaneo_settings_load();
	{ extern int ayaneo_resume_pending(void); if (ayaneo_resume_pending()) goto skip_boot_anim; }
	if (ayaneo_gbc_select_held() || ayaneo_get_skip_boot()) {
skip_boot_anim:
		s_anim_complete = 1;
		s_rainbow_exited = 1;
		return;
	}
#endif
	/*
	 * HIGH_PRIORITY so the thread gets its quick per-frame present in promptly
	 * even during the CPU/bandwidth-heavy boot-image verify (fewer dropped
	 * frames). This is only safe because the loop now does an unconditional
	 * thread_sleep() every frame - the earlier HIGH_PRIORITY crash was from a
	 * render loop that could spin without sleeping and starve the boot thread
	 * (watchdog reset). With the guaranteed sleep, boot always gets the CPU back.
	 */
	t = thread_create("ayaneo_rainbow", &ayaneo_rainbow_thread, NULL,
			  HIGH_PRIORITY, DEFAULT_STACK_SIZE);
	if (t)
		thread_resume(t);
#ifdef AYANEO_DEBUG_LOGGING
	else
		dprintf(CRITICAL, "AYANEO_RAINBOW: thread_create failed\n");
#endif
}

void video_rainbow_boot_stop(void)
{
	int guard = 0;

	if (s_rainbow_exited)
		return;

	/*
	 * Boot is ready. Request the fade and wait for the player to finish the
	 * full animation and its code fade (s_anim_complete) before handing off, so
	 * the kernel never cuts the animation short. Bounded (~8s) so a decode stall
	 * can never hang boot forever - the animation itself runs ~4-5s.
	 */
	s_fade_request = 1;
	while (!s_rainbow_exited && !s_anim_complete && guard++ < 400)
		thread_sleep(20);

	guard = 0;
	s_rainbow_stop = 1;
	while (!s_rainbow_exited && guard++ < 500)
		thread_sleep(2);

#if defined(AYANEO_BOOT_AUDIO) && !defined(AYANEO_GBC) && !defined(AYANEO_GBA)
	/* make sure the AFE/codec is quiet before the kernel re-inits audio.
	 * In the GBC build there is no kernel: let the chime play to completion
	 * (the emulator waits for it before bringing up its own audio). */
	ayaneo_boot_audio_stop();
#endif
}
#endif /* AYANEO_RAINBOW_BOOT */

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
void mt_ext_disp_update(UINT32 x, UINT32 y, UINT32 width, UINT32 height)
{
	unsigned int va = ext_fb_addr;

	arch_clean_cache_range((unsigned int)ext_fb_addr, EXT_DISP_GetFBRamSize());
	external_display_trigger(TRUE);

	if (!external_display_is_video_mode()) {
		/*video mode no need to wait*/
		dprintf(CRITICAL,"cmd mode trigger wait\n");
		mdelay(30);
	}
}
#endif

static void mt_disp_adjusting_hardware_addr(void)
{
	dprintf(CRITICAL,"mt_disp_adjusting_hardware_addr fb_offset_logo = %d fb_size=%d\n",fb_offset_logo,fb_size);
	if (fb_offset_logo == 0) {
		mt_get_fb_addr();
		memcpy(fb_addr,(void *)((UINT32)fb_addr + 3 * fb_size),fb_size);
		mt_disp_update(0, 0, CFG_DISPLAY_WIDTH, CFG_DISPLAY_HEIGHT);
	}
}

UINT32 mt_disp_get_lcd_time(void)
{
	static unsigned int fps = 0;

	if (!fps) {
		fps = primary_display_get_vsync_interval();

		dprintf(CRITICAL, "%s, fps=%d\n", __func__, fps);

		if (!fps)
			fps = 6000;
	}

	return fps;
}

int mt_disp_config_frame_buffer(void *fdt)
{
	extern unsigned int g_fb_base;
	extern unsigned int g_fb_size;
	unsigned int fb_base_h =0;
	unsigned int fb_base_l = g_fb_base & 0xffffffff;
	int offset;
	int ret;

	fb_base_h = cpu_to_fdt32(fb_base_h);
	fb_base_l = cpu_to_fdt32(fb_base_l);
	g_fb_size = cpu_to_fdt32(g_fb_size);
	/* placed in the DT chosen node */
	offset = fdt_path_offset(fdt, "/chosen");
	if (offset < 0) {
		return offset;
	}
	ret = fdt_setprop(fdt, offset, "atag,videolfb-fb_base_h", &fb_base_h, sizeof(fb_base_h));
	ret = fdt_setprop(fdt, offset, "atag,videolfb-fb_base_l", &fb_base_l, sizeof(fb_base_l));
	ret = fdt_setprop(fdt, offset, "atag,videolfb-vramSize", &g_fb_size, sizeof(g_fb_size));

	return ret;
}

// Attention: this api indicates whether the lcm is connected
int DISP_IsLcmFound(void)
{
	return primary_display_is_lcm_connected();
}

const char* mt_disp_get_lcm_id(void)
{
	return primary_display_get_lcm_name();
}

#if (MTK_DUAL_DISPLAY_SUPPORT == 2)
const char* mt_ext_disp_get_lcm_id(void)
{
	return external_display_get_lcm_name();
}

int EXT_DISP_IsLcmFound(void)
{
	return external_display_is_lcm_connected();
}
#endif

void disp_get_fb_address(UINT32 *fbVirAddr, UINT32 *fbPhysAddr)
{
	*fbVirAddr = (UINT32)fb_addr;
	*fbPhysAddr = (UINT32)fb_addr;
}

UINT32 mt_disp_get_redoffset_32bit(void)
{
	return redoffset_32bit;
}


// ---------------------------------------------------------------------------
//  Export Functions - Console
// ---------------------------------------------------------------------------

#ifdef CONFIG_CFB_CONSOLE
//  video_hw_init -- called by drv_video_init() for framebuffer console

void *video_hw_init (void)
{
	static GraphicDevice s_mt65xx_gd;

	memset(&s_mt65xx_gd, 0, sizeof(GraphicDevice));

	s_mt65xx_gd.frameAdrs  = (UINT32)fb_addr+fb_size;
	s_mt65xx_gd.winSizeX   = CFG_DISPLAY_WIDTH;
	s_mt65xx_gd.winSizeY   = CFG_DISPLAY_HEIGHT;
	s_mt65xx_gd.gdfIndex   = CFB_X888RGB_32BIT;
	dprintf(0, "s_mt65xx_gd.gdfIndex=%d", s_mt65xx_gd.gdfIndex);
	s_mt65xx_gd.gdfBytesPP = CFG_DISPLAY_BPP / 8;
	s_mt65xx_gd.memSize    = s_mt65xx_gd.winSizeX * s_mt65xx_gd.winSizeY * s_mt65xx_gd.gdfBytesPP;

	return &s_mt65xx_gd;
}


void video_set_lut(unsigned int index,  /* color number */
                   unsigned char r,     /* red */
                   unsigned char g,     /* green */
                   unsigned char b)     /* blue */
{
	dprintf(CRITICAL, "%s\n", __func__);

}

#endif  // CONFIG_CFB_CONSOLE
