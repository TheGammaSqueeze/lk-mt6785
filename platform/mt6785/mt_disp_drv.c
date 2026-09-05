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

/* ---- primitive drawing into a 32-bit BGRA back buffer (used by the menu) ---- */

/* filled rectangle, clipped to the panel */
void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
		 int x, int y, int w, int h, unsigned int argb)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
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
	unsigned int Wd = CFG_DISPLAY_WIDTH, Hd = CFG_DISPLAY_HEIGHT;
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
	*pitch_w = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT);
	*W = CFG_DISPLAY_WIDTH;
	*H = CFG_DISPLAY_HEIGHT;
	return (unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? fb_size : 0));
}

/* Get the currently DISPLAYED (front) buffer - the one being scanned out, i.e.
 * whatever was last presented. Used by the fastboot debug channel to screenshot
 * the live panel content while the menu/game runs. present() flips s_fb_flip
 * AFTER latching, so the displayed buffer is the opposite of the current back. */
const unsigned int *ayaneo_canvas_front(unsigned int *pitch_w, unsigned int *W, unsigned int *H)
{
	*pitch_w = ALIGN_TO(CFG_DISPLAY_WIDTH, MTK_FB_ALIGNMENT);
	*W = CFG_DISPLAY_WIDTH;
	*H = CFG_DISPLAY_HEIGHT;
	return (const unsigned int *)((unsigned char *)fb_addr + (s_fb_flip ? 0 : fb_size));
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

	arch_clean_cache_range((unsigned int)((unsigned char *)fb_addr +
			       (s_fb_flip ? fb_size : 0)), fb_size);
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

				if (!filt) {
					/* Fast path (no LCD filter, the common case): the whole
					 * GBC_SCALE x GBC_SCALE block is the solid pixel, so write it
					 * with no per-subpixel branch. Byte-identical to the filtered
					 * path below when filt==0. This is the dominant per-frame cost
					 * (measured ~7.6 ms/frame at 600 MHz), so hoisting the branch
					 * out of ~1M inner iterations is a large win. */
					for (iy = 0; iy < GBC_SCALE; iy++) {
						unsigned int *o = dst +
							(yoff + sy * GBC_SCALE + iy) * pitch_w +
							(xoff + sx * GBC_SCALE);
						for (ix = 0; ix < GBC_SCALE; ix++)
							o[ix] = px;
					}
				} else
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
	if (gbc_benchmark_on()) {		/* FPS counter, top-left of the game area */
		char s[16]; int fps = gbc_get_fps(), n = 0, t[8], k = 0;
		s[n++]='F'; s[n++]='P'; s[n++]='S'; s[n++]=':'; s[n++]=' ';
		if (fps <= 0) s[n++]='0';
		else { while (fps) { t[k++]='0'+fps%10; fps/=10; } while (k) s[n++]=t[--k]; }
		s[n]=0;
		ayaneo_fill(dst, pitch_w, xoff + 4, yoff + 4, 200, 28, 0xFF000000u);
		ayaneo_text(dst, pitch_w, xoff + 8, yoff + 6, 2, 0xFF30FF60u, s);
	}
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

	for (sy = 0; sy < SH; sy++) {
		const unsigned short *srow = pix + sy * SW;
		for (sx = 0; sx < SW; sx++) {
			unsigned int v = srow[sx];
			unsigned int r = ((v >> 11) & 0x1f) << 3;
			unsigned int g = ((v >> 5) & 0x3f) << 2;
			unsigned int b = (v & 0x1f) << 3;
			unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
			unsigned int dk = 0xFF000000u | ((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
			if (!filt) {
				for (iy = 0; iy < SC; iy++) {
					unsigned int *o = dst + (yoff + sy * SC + iy) * pitch_w + (xoff + sx * SC);
					for (ix = 0; ix < SC; ix++) o[ix] = px;
				}
			} else {
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
void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int *b = (unsigned int *)MENU_OVERLAY_PA;
	if (open && paint) {
		if (!g_overlay_active || s_overlay_dirty) {
			/* Clear to transparent (alpha=0) then draw the opaque panel. Full-screen clear each
			 * repaint handles GBA/GBC swapping between differently-sized panels (menu vs palette/
			 * aspect pickers) with no stale opaque pixels - but only on a real content change.
			 * Painted BEFORE the layer is enabled on the open frame so no stale buffer flashes. */
			memset(b, 0, W * H * 4u);
			paint(b, W, W, H);
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
void ayaneo_rsz_present(const unsigned short *pix, unsigned int sw, unsigned int sh, unsigned int spitch_px,
			unsigned int dw, unsigned int dh, unsigned int xoff, unsigned int yoff,
			int filt, int wait_vsync, const unsigned short *cc_lut)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int t0 = gpt4_get_current_tick();

	if (!pix || !fb_addr || !sw || !sh) return;
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
	 * dim the last of each pv rows; grid (filt>=2) also dims the last of each ph columns. */
	{
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
				if (filt >= 2 && ph > 0u)   /* grid: dim last column of this pixel's run */
					o[ph - 1u] = 0xFF000000u | ((r >> 1) << 16) | ((g >> 1) << 8) | (b >> 1);
			}
			for (r_ = 1u; r_ < pv; r_++)
				memcpy(drow + r_ * fbpitch, drow, iw * 4u);   /* replicate the widened row */
			if (filt >= 1 && pv > 0u) {   /* scanline: dim the last of the pv rows */
				unsigned int *lr = drow + (pv - 1u) * fbpitch;
				for (x = 0; x < iw; x++) {
					unsigned int v = lr[x];
					lr[x] = 0xFF000000u | ((((v >> 16) & 0xffu) >> 1) << 16) |
					        ((((v >> 8) & 0xffu) >> 1) << 8) | ((v & 0xffu) >> 1);
				}
			}
		}
	}
	ayaneo_hud_draw(vbuf, fbpitch, 0, 0, (int)iw, (int)ih);   /* FF/RW speed badge (scaled with the game) */
	arch_clean_cache_range((unsigned int)vbuf, ih * fbpitch * 4u);

	/* Detect geometry changes (source res or output rect - dw/dh uniquely encode the aspect mode)
	 * - only then is the RSZ (a clock-only LK stub, so it persists across config_input) reprogrammed. */
	int geom_changed = (!s_rsz_setup || s_rsz_sw != sw || s_rsz_sh != sh ||
	                    s_rsz_dw != dw || s_rsz_dh != dh);

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
		primary_display_config_input(&in);
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
	g_rsz_show_us = (gpt4_get_current_tick() - t0) / 13u;
	if (wait_vsync) priamry_display_wait_for_vsync();
	s_fb_flip ^= 1;
}

/* SNES RSZ wrapper: compute the target rect from the SNES aspect options, then present via the
 * shared hardware-RSZ path. */
static void ayaneo_snes_show_frame_rsz(const unsigned short *pix, unsigned int sw, unsigned int sh,
				       unsigned int spitch_px)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	extern volatile unsigned g_snes_aspect_x1000, g_snes_show_us;
	extern volatile int g_snes_stretch;
	extern int ayaneo_get_lcd_filter(void);
	extern int snes_benchmark_on(void);
	unsigned int sy_scale = (sh <= 240u) ? 4u : 2u, dw, dh;
	if (!pix || !fb_addr) return;
	if (g_snes_stretch) {
		dw = W; dh = H;
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
		if (g_snes_rsz) {
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
	arch_clean_cache_range((unsigned int)dst, H * pitch_w * 4);
	unsigned int t_flush1 = gpt4_get_current_tick();
	g_snes_flush_us = (t_flush1 - t_flush0) / 13u;               /* just the 4.9MB clean */
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
void ayaneo_genesis_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px)
{
	extern unsigned int gpt4_get_current_tick(void);
	extern volatile int g_genesis_aspect;          /* 0=Pixel 1=Fit 2=Stretch (genesis_sd_run.c) */
	extern volatile unsigned g_genesis_aspect_x1000;
	extern volatile int g_genesis_filter;          /* 0=off 1=scanlines 2/3=grid */
	unsigned int t0 = gpt4_get_current_tick();
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch_w = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int scale, s2, dw, dh, xoff, yoff, sx, sy, ix, iy;
	unsigned int *dst, dpa;
	int filt = g_genesis_filter;

	if (!fb_addr || !pix || !sw || !sh) return;

	/* Aspect modes: Fit / Stretch via the hardware RSZ; Pixel (0) uses the sharp integer CPU blit.
	 * Genesis is 4:3 on a 4:3 panel, so Fit uses the core's aspect (g_genesis_aspect_x1000, ~4:3)
	 * and Stretch fills the panel. RSZ stays on under the live overlay menu; Pixel/exit restores 1:1. */
	if (g_genesis_aspect != 0) {
		unsigned int dwr, dhr, a;
		if (g_genesis_aspect == 2) { dwr = W; dhr = H; }
		else { a = g_genesis_aspect_x1000; if (!a) a = 1333u;
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

	for (sy = 0; sy < sh; sy++) {
		const unsigned short *srow = pix + sy * spitch_px;
		for (sx = 0; sx < sw; sx++) {
			unsigned int v = srow[sx];
			unsigned int r = ((v >> 11) & 0x1f) << 3;
			unsigned int g = ((v >> 5) & 0x3f) << 2;
			unsigned int b = (v & 0x1f) << 3;
			unsigned int px = 0xFF000000u | (r << 16) | (g << 8) | b;
			if (!filt) {
				for (iy = 0; iy < scale; iy++) {
					unsigned int *o = dst + (yoff + sy * scale + iy) * pitch_w + (xoff + sx * scale);
					for (ix = 0; ix < scale; ix++) o[ix] = px;
				}
			} else {
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
	arch_clean_cache_range((unsigned int)dst, H * pitch_w * 4);
	g_dbg_blit_us = (gpt4_get_current_tick() - t0) / 13u;
	ayaneo_present(dpa, W, H, pitch_w);
	priamry_display_wait_for_vsync();
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

void ayaneo_gba_punch_prerender(const unsigned short *pix)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	extern volatile int g_gba_aspect;
	if (!pix) return;
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

void ayaneo_gba_punch_frame_pre(const unsigned int *snap, int radius)
{
	unsigned int W = CFG_DISPLAY_WIDTH, H = CFG_DISPLAY_HEIGHT;
	unsigned int pitch = ALIGN_TO(W, MTK_FB_ALIGNMENT);
	unsigned int *dst;
	unsigned int dpa;
	if (!fb_addr || !snap) return;
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
				anim_blit(rgb, disp_va[disp_i & 1], sw, sh, dw, dh,
					  xoff, yoff, pitch_w, 256, crop_y, scaled_h);
				ayaneo_present(disp_pa[disp_i & 1], W, H, pitch_w);
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
	if (ayaneo_gbc_select_held() || ayaneo_get_skip_boot()) {
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
