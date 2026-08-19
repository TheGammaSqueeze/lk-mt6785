/*
 * ST7703 HD720 (1280x960) DSI video-mode panel for MT6785 (k85v1_64).
 * AYANEO Pocket AIR Mini.
 *
 * Panel parameters and the register init sequence were recovered from the
 * stock signed lk image (st7703_hd720_lcm_drv). The panel uses a page-based
 * register scheme: CMD 0xEE selects the active register page, subsequent
 * writes target that page.
 *
 * Drive voltages are exposed as the ST7703_* defines below. They default to
 * the stock values. Tune ONE register at a time and verify on the panel;
 * over-driving does not fail safe. Known ceilings: AVEE 0x60, VGH/VGL 0x78,
 * AVDD 0xFF.
 */

#ifdef BUILD_LK
#include <string.h>
#include <platform/mt_gpio.h>
#include <platform/mt_pmic.h>
#include <platform/upmu_common.h>
#include <platform/mt_i2c.h>
#endif

#include "lcm_drv.h"

#define FRAME_WIDTH   (1280)
#define FRAME_HEIGHT  (960)

/*
 * Tunable drive voltages, set to the tuned AYANEO Pocket Air Mini values
 * (stock values in comments). Tune ONE register at a time and verify;
 * over-driving does not fail safe. Ceilings: AVEE 0x60, VGH/VGL 0x78, AVDD 0xFF.
 */
#define ST7703_VGH    0x78	/* stock 0x58, ceiling 0x78 */
#define ST7703_VGL    0x78	/* stock 0x58, ceiling 0x78 */
#define ST7703_CPUMP  0x48	/* charge pump freq, stock 0x32 */
#define ST7703_AVDD   0xFF	/* stock 0xE0 */
#define ST7703_AVEE   0x60	/* stock 0x20, ceiling 0x60 (0x70 flickers) */

#define GPIO_OUT_ONE  1
#define GPIO_OUT_ZERO 0

static LCM_UTIL_FUNCS lcm_util;

#define SET_RESET_PIN(v) (lcm_util.set_reset_pin((v)))
#define UDELAY(n) (lcm_util.udelay(n))
#define MDELAY(n) (lcm_util.mdelay(n))

#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
	lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define dsi_set_cmdq(pdata, queue_size, force_update) \
	lcm_util.dsi_set_cmdq(pdata, queue_size, force_update)

#define REGFLAG_DELAY         0xFC
#define REGFLAG_END_OF_TABLE  0xFD

struct LCM_setting_table {
	unsigned char cmd;
	unsigned char count;
	unsigned char para_list[64];
};

static struct LCM_setting_table lcm_initialization_setting[] = {
	{0xEE, 1, {0x01} }	/* page 0x01 */,
	{0xEA, 1, {0x07} },
	{0xEB, 1, {0x12} },
	{0x05, 1, {0x19} },
	{0x0A, 1, {0x86} },
	{0x14, 1, {ST7703_VGH} } /* VGH */,
	{0x15, 1, {ST7703_VGL} } /* VGL */,
	{0x17, 1, {ST7703_CPUMP} } /* charge pump freq */,
	{0x28, 1, {0x1F} },
	{0x29, 1, {0x29} },
	{0x2A, 1, {0x63} },
	{0x2F, 1, {0xF3} },
	{0xEE, 1, {0x02} }	/* page 0x02 */,
	{0x39, 1, {0x70} },
	{0x00, 1, {0x00} },
	{0x01, 1, {0x0D} },
	{0x02, 1, {0x14} },
	{0x03, 1, {0x09} },
	{0x04, 1, {0x10} },
	{0x05, 1, {0x4C} },
	{0x06, 1, {0x0B} },
	{0x07, 1, {0x11} },
	{0x08, 1, {0x0E} },
	{0x09, 1, {0x0D} },
	{0x0A, 1, {0x11} },
	{0x0B, 1, {0x5F} },
	{0x0C, 1, {0x14} },
	{0x0D, 1, {0x18} },
	{0x0E, 1, {0x3E} },
	{0x0F, 1, {0x3D} },
	{0x10, 1, {0x3F} },
	{0x20, 1, {0x00} },
	{0x21, 1, {0x0B} },
	{0x22, 1, {0x12} },
	{0x23, 1, {0x03} },
	{0x24, 1, {0x08} },
	{0x25, 1, {0x40} },
	{0x26, 1, {0x0B} },
	{0x27, 1, {0x09} },
	{0x28, 1, {0x0E} },
	{0x29, 1, {0x0D} },
	{0x2A, 1, {0x11} },
	{0x2B, 1, {0x5F} },
	{0x2C, 1, {0x14} },
	{0x2D, 1, {0x18} },
	{0x2E, 1, {0x3E} },
	{0x2F, 1, {0x3D} },
	{0x30, 1, {0x3F} },
	{0xEE, 1, {0x04} }	/* page 0x04 */,
	{0x00, 1, {0x01} },
	{0x01, 1, {0x01} },
	{0x02, 1, {ST7703_AVDD} } /* AVDD */,
	{0x03, 1, {0x05} },
	{0x04, 1, {0x00} },
	{0x06, 1, {0x14} },
	{0x07, 1, {0x05} },
	{0x08, 1, {0x12} },
	{0x09, 1, {ST7703_AVEE} } /* AVEE */,
	{0x0A, 1, {0x0F} },
	{0x0B, 1, {0x04} },
	{0x20, 1, {0x40} },
	{0x2A, 1, {0x00} },
	{0x40, 1, {0x80} },
	{0x41, 1, {0x60} },
	{0xEE, 1, {0x05} }	/* page 0x05 */,
	{0x00, 1, {0x05} },
	{0x01, 1, {0x09} },
	{0x02, 1, {0x05} },
	{0x03, 1, {0x05} },
	{0x07, 1, {0x01} },
	{0x08, 1, {0x05} },
	{0x09, 1, {0x00} },
	{0x10, 1, {0x08} },
	{0x11, 1, {0x0C} },
	{0x12, 1, {0x25} },
	{0x13, 1, {0x05} },
	{0x19, 1, {0x90} },
	{0x1A, 1, {0x77} },
	{0x23, 1, {0x00} },
	{0x30, 1, {0x01} },
	{0x31, 1, {0x01} },
	{0x32, 1, {0x00} },
	{0x33, 1, {0x14} },
	{0x34, 1, {0x14} },
	{0x35, 1, {0xB4} },
	{0x36, 1, {0x01} },
	{0x37, 1, {0x01} },
	{0x38, 1, {0x00} },
	{0x39, 1, {0x14} },
	{0x3A, 1, {0x14} },
	{0x40, 1, {0x00} },
	{0x41, 1, {0x00} },
	{0x43, 1, {0x11} },
	{0x44, 1, {0x01} },
	{0x45, 1, {0x81} },
	{0x46, 1, {0x06} },
	{0x47, 1, {0x03} },
	{0xEE, 1, {0x06} }	/* page 0x06 */,
	{0x00, 1, {0x23} },
	{0x01, 1, {0x01} },
	{0x02, 1, {0x04} },
	{0x06, 1, {0xCD} },
	{0x08, 1, {0x67} },
	{0x09, 1, {0x45} },
	{0x0A, 1, {0x23} },
	{0x0B, 1, {0x01} },
	{0xEE, 1, {0x07} }	/* page 0x07 */,
	{0x00, 1, {0x14} },
	{0x01, 1, {0x14} },
	{0x02, 1, {0x16} },
	{0x03, 1, {0x16} },
	{0x04, 1, {0x10} },
	{0x05, 1, {0x10} },
	{0x06, 1, {0x12} },
	{0x07, 1, {0x12} },
	{0x08, 1, {0x0D} },
	{0x09, 1, {0x0D} },
	{0x0A, 1, {0x00} },
	{0x0B, 1, {0x00} },
	{0x0C, 1, {0x0C} },
	{0x0D, 1, {0x0C} },
	{0x0E, 1, {0x04} },
	{0x0F, 1, {0x04} },
	{0x10, 1, {0x3C} },
	{0x11, 1, {0x3C} },
	{0x12, 1, {0x20} },
	{0x13, 1, {0x20} },
	{0x14, 1, {0x21} },
	{0x15, 1, {0x21} },
	{0x20, 1, {0x15} },
	{0x21, 1, {0x15} },
	{0x22, 1, {0x17} },
	{0x23, 1, {0x17} },
	{0x24, 1, {0x11} },
	{0x25, 1, {0x11} },
	{0x26, 1, {0x13} },
	{0x27, 1, {0x13} },
	{0x28, 1, {0x0D} },
	{0x29, 1, {0x0D} },
	{0x2A, 1, {0x01} },
	{0x2B, 1, {0x01} },
	{0x2C, 1, {0x0C} },
	{0x2D, 1, {0x0C} },
	{0x2E, 1, {0x04} },
	{0x2F, 1, {0x04} },
	{0x30, 1, {0x3C} },
	{0x31, 1, {0x3C} },
	{0x32, 1, {0x20} },
	{0x33, 1, {0x20} },
	{0x34, 1, {0x21} },
	{0x35, 1, {0x21} },
	{0xEE, 1, {0x08} }	/* page 0x08 */,
	{0x12, 1, {0xDA} },
	{0x13, 1, {0x9B} },
	{0x18, 1, {0x00} },
	{0x20, 1, {0x00} },
	{0x22, 1, {0x69} },
	{0x2C, 1, {0x20} },
	{0x4B, 1, {0xA0} },
	{0x61, 1, {0x20} },
	{0xEE, 1, {0x0F} }	/* page 0x0f */,
	{0x00, 1, {0x01} },
	{0xEE, 1, {0x00} }	/* page 0x00 */,
	{0xEA, 1, {0x00} },
	{0xEB, 1, {0x00} },
	{0x36, 1, {0x00} },
	{0x11, 1, {0x00} },
	{REGFLAG_DELAY, 60, {} },
	{0x29, 1, {0x00} },
	{REGFLAG_DELAY, 10, {} },
	{REGFLAG_END_OF_TABLE, 0x00, {} }
};

static void push_table(struct LCM_setting_table *table, unsigned int count,
		       unsigned char force_update)
{
	unsigned int i;
	unsigned int cmd;

	for (i = 0; i < count; i++) {
		cmd = table[i].cmd;
		switch (cmd) {
		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			dsi_set_cmdq_V2(cmd, table[i].count, table[i].para_list,
					force_update);
		}
	}
}

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_params(LCM_PARAMS *params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type = LCM_TYPE_DSI;

	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;

	params->dsi.mode = SYNC_PULSE_VDO_MODE;
	params->dsi.LANE_NUM = LCM_FOUR_LANE;

	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;

	params->dsi.intermediat_buffer_num = 2;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;

	params->dsi.vertical_sync_active = 8;
	params->dsi.vertical_backporch = 8;
	params->dsi.vertical_frontporch = 16;
	params->dsi.vertical_active_line = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active = 30;
	params->dsi.horizontal_backporch = 60;
	params->dsi.horizontal_frontporch = 60;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;

	params->dsi.word_count = FRAME_WIDTH * 3;

	params->dsi.PLL_CLOCK = 266;
}

static void lcm_init_lcm(void)
{
#ifdef BUILD_LK
	printf("[LK/LCM] st7703_hd720 lcm_init() enter\n");

	SET_RESET_PIN(1);
	MDELAY(10);
	SET_RESET_PIN(0);
	MDELAY(10);
	SET_RESET_PIN(1);
	MDELAY(120);

	push_table(lcm_initialization_setting,
		   sizeof(lcm_initialization_setting) /
		   sizeof(struct LCM_setting_table), 1);
#endif
}

void lcm_suspend(void)
{
}

void lcm_resume(void)
{
	lcm_init_lcm();
}

LCM_DRIVER st7703_hd720_dsi_vdo_lcm_drv = {
	/*
	 * This name is passed to the kernel (videolfb lcmname) and MUST match the
	 * LCM driver compiled into the kernel (st7703_hd720_lcm_drv). If it differs,
	 * the kernel's disp_lcm_probe returns NULL and mtkfb Oopses in
	 * layering_rule_init. Do not rename to match the directory.
	 */
	.name = "st7703_hd720_lcm_drv",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init_lcm,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
};
