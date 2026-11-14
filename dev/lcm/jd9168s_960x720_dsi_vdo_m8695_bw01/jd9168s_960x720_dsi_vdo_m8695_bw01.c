/*
 * Jadard JD9168S DSI video mode panel, 960x720 native landscape (4:3), the display of the Konkr
 * Pocket Block (MT8786 on the mt6768 platform, board m8695_bw01). No rotation: the frame buffer is
 * scanned out as it is drawn.
 *
 * Transcribed from the stock Block LK (m8695_bw01, 2026-10-14): get_params 0x4c432b70, init
 * 0x4c432dc4, init_power 0x4c432eec, compare_id 0x4c432ce8, init table at 0x4c4a08e8 (29
 * entries), suspend table at 0x4c4a1110. Geometry, porches and lane rate agree with the kernel's
 * panel-jd9168s_960x720_dsi_vdo_m8695 mode table (45104 kHz pixel clock, 59.9995 Hz).
 *
 * Power: the PMIC's VLDO28 (register 0x1d32 bit 0) for the panel's IO rail, then GPIO165 and
 * GPIO169 raise the OCP2131 bias, which is set to its +-bias level over I2C0 (0x3e, registers 0
 * and 1 to 0x0f). Reset is GPIO45; the tearing signal on GPIO44 is not used in video mode.
 *
 * Two glass builds ship. After a reset the stock code unlocks the vendor pages and reads register
 * 0xb2; when its second byte is 'F' the fourth init entry is sent to 0xb2 instead of 0xb3 (same
 * two bytes 00 9b). The probe is kept for that reason.
 */

#ifdef BUILD_LK
#include <string.h>
#include <debug.h>
#include <platform/mt_gpio.h>
#include <platform/mt_i2c.h>
#include <platform/mt_pmic.h>
#endif

#include "lcm_drv.h"

#define FRAME_WIDTH   (960)
#define FRAME_HEIGHT  (720)

#define GPIO_LCD_RST     (45u | 0x80000000u)    /* panel reset, active low */
#define GPIO_BIAS_ENP    (165u | 0x80000000u)   /* OCP2131 positive rail enable */
#define GPIO_BIAS_ENN    (169u | 0x80000000u)   /* OCP2131 negative rail enable */

#define BIAS_I2C_ADDR    0x3e                   /* OCP2131 on I2C0 */
#define BIAS_LEVEL       0x0f                   /* the stock +-bias setting, both rails */

#define PMIC_VLDO28_EN   0x1d32                 /* bit 0: the panel IO rail */

#define REGFLAG_DELAY         0xfffc
#define REGFLAG_END_OF_TABLE  0xfffd

/* the init entry the glass probe may move from 0xb3 to 0xb2 */
#define GLASS_ENTRY      3

static LCM_UTIL_FUNCS lcm_util;
volatile int g_jd9168s_glass_f = -1;   /* the last probe: 1 when register 0xb2 read 'F', -1 not read */

#define MDELAY(n) (lcm_util.mdelay(n))

struct jd_cmd {
	unsigned int cmd;
	unsigned char count;
	unsigned char para[38];
};

/* The stock sequence: vendor pages unlocked (0xdf), page 0, panel settings and gamma, page 2,
 * back to page 0, sleep out, display on. */
static struct jd_cmd jd_init[] = {
	{ 0xdf, 3, { 0x91, 0x68, 0xf9 } },
	{ 0xde, 1, { 0x00 } },
	{ 0xde, 1, { 0x00 } },
	{ 0xb3, 2, { 0x00, 0x9b } },
	{ 0xc1, 6, { 0x00, 0x10, 0x00, 0x00, 0x00, 0x00 } },
	{ 0xbb, 7, { 0x02, 0x14, 0x08, 0x24, 0x19, 0x44, 0x44 } },
	{ 0xbe, 2, { 0x1a, 0xf2 } },
	{ 0xc3, 14, { 0x10, 0x07, 0x5a, 0x07, 0x5a, 0x05, 0x05, 0x05, 0x05, 0x1b, 0x1b, 0x31,
		      0x08, 0xd4 } },
	{ 0xc4, 7, { 0x10, 0x68, 0xf0, 0xea, 0x09, 0x07, 0x14 } },
	{ 0xce, 23, { 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
		      0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03 } },
	{ 0xcf, 9, { 0x00, 0x00, 0x8d, 0x01, 0x7c, 0x00, 0x8d, 0x01, 0x7c } },
	{ 0xd0, 23, { 0x00, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x02,
		      0x00, 0x04, 0x06, 0x08, 0x0a, 0x1e, 0x1e, 0x1f, 0x1f, 0x11, 0x1f } },
	{ 0xd1, 23, { 0x00, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x03,
		      0x01, 0x05, 0x07, 0x09, 0x0b, 0x1e, 0x1e, 0x1f, 0x1f, 0x10, 0x1f } },
	{ 0xd2, 23, { 0x00, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x01,
		      0x03, 0x0b, 0x09, 0x07, 0x05, 0x1f, 0x1e, 0x1f, 0x1f, 0x11, 0x1e } },
	{ 0xd3, 23, { 0x00, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x00,
		      0x02, 0x0a, 0x08, 0x06, 0x04, 0x1f, 0x1e, 0x1f, 0x1f, 0x10, 0x1e } },
	{ 0xd4, 37, { 0x30, 0x00, 0x00, 0x06, 0x00, 0x08, 0x00, 0x04, 0xc2, 0x00, 0x00, 0x03,
		      0x03, 0x00, 0x00, 0x85, 0xb8, 0x40, 0x04, 0x0d, 0x0d, 0x00, 0x80, 0x04,
		      0x40, 0x04, 0x0d, 0x0d, 0x00, 0x00, 0x14, 0x04, 0x14, 0x05, 0xb1, 0x05,
		      0xb1 } },
	{ 0xd5, 5, { 0x68, 0x73, 0x00, 0x0a, 0x08 } },
	{ 0xb7, 6, { 0x00, 0xff, 0x01, 0x00, 0xff, 0x01 } },
	{ 0xc8, 38, { 0x7f, 0x69, 0x59, 0x4c, 0x47, 0x37, 0x3b, 0x23, 0x42, 0x41, 0x3e, 0x5d,
		      0x4b, 0x55, 0x47, 0x41, 0x34, 0x1f, 0x08, 0x7f, 0x69, 0x59, 0x4c, 0x47,
		      0x37, 0x3b, 0x23, 0x34, 0x33, 0x34, 0x53, 0x41, 0x49, 0x3b, 0x39, 0x2c,
		      0x1f, 0x08 } },
	{ 0xde, 1, { 0x02 } },
	{ 0xbb, 4, { 0x00, 0x5b, 0x5c, 0x41 } },
	{ 0xc1, 1, { 0x15 } },
	{ 0xe7, 1, { 0x01 } },
	{ 0xde, 1, { 0x00 } },
	{ 0x11, 0, { 0 } },
	{ REGFLAG_DELAY, 120, { 0 } },
	{ 0x29, 0, { 0 } },
	{ REGFLAG_DELAY, 20, { 0 } },
	{ REGFLAG_END_OF_TABLE, 0, { 0 } },
};

static struct jd_cmd jd_sleep[] = {
	{ 0x28, 0, { 0 } },
	{ REGFLAG_DELAY, 20, { 0 } },
	{ 0x10, 0, { 0 } },
	{ REGFLAG_DELAY, 120, { 0 } },
	{ REGFLAG_END_OF_TABLE, 0, { 0 } },
};

static void send_cmds(const struct jd_cmd *t)
{
	for (; t->cmd != REGFLAG_END_OF_TABLE; t++) {
		if (t->cmd == REGFLAG_DELAY)
			MDELAY(t->count);
		else
			lcm_util.dsi_set_cmdq_V2((unsigned char)t->cmd, t->count, (unsigned char *)t->para, 1);
	}
}

#ifdef BUILD_LK
static void pin_out(unsigned int pin, int level)
{
	mt_set_gpio_mode(pin, GPIO_MODE_00);
	mt_set_gpio_dir(pin, GPIO_DIR_OUT);
	mt_set_gpio_out(pin, level ? GPIO_OUT_ONE : GPIO_OUT_ZERO);
}

static int bias_write(unsigned char reg, unsigned char val)
{
	mt_i2c i2c;
	unsigned char buf[2] = { reg, val };

	memset(&i2c, 0, sizeof(i2c));
	i2c.id = I2C0;
	i2c.addr = BIAS_I2C_ADDR;
	i2c.mode = ST_MODE;
	i2c.speed = 100;
	return i2c_write(&i2c, buf, 2);
}
#endif

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

/* The stock values; the stock code clears the whole structure first and so does this one. */
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
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.packet_size = 256;

	/* vtotal 737 (2 + 6 + 720 + 9), htotal 1020 (20 + 20 + 960 + 20): 60.0 Hz at 280 Mbps a lane */
	params->dsi.vertical_sync_active = 2;
	params->dsi.vertical_backporch = 6;
	params->dsi.vertical_frontporch = 9;
	params->dsi.vertical_active_line = FRAME_HEIGHT;
	params->dsi.horizontal_sync_active = 20;
	params->dsi.horizontal_backporch = 20;
	params->dsi.horizontal_frontporch = 20;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;

	params->dsi.PLL_CLOCK = 140;
	params->dsi.data_rate = 280;

	/* the stock ESD check entry, left disabled as there: register 0x0a reads 0x9d */
	params->dsi.esd_check_enable = 0;
	params->dsi.customization_esd_check_enable = 0;
	params->dsi.lcm_esd_check_table[0].cmd = 0x0a;
	params->dsi.lcm_esd_check_table[0].count = 1;
	params->dsi.lcm_esd_check_table[0].para_list[0] = 0x9d;
}

static void lcm_init_power(void)
{
#ifdef BUILD_LK
	dprintf(CRITICAL, "[LK/LCM] jd9168s power\n");
	pmic_config_interface(PMIC_VLDO28_EN, 1, 1, 0);
	pin_out(GPIO_BIAS_ENP, 1);
	pin_out(GPIO_BIAS_ENN, 1);
	MDELAY(10);
	if (bias_write(0x00, BIAS_LEVEL) != 0 || bias_write(0x01, BIAS_LEVEL) != 0)
		dprintf(CRITICAL, "[LK/LCM] jd9168s: OCP2131 bias write failed\n");
	MDELAY(10);
#endif
}

/* Reset the panel, unlock the vendor pages and read register 0xb2: decides the glass entry. */
static unsigned int lcm_compare_id(void)
{
#ifdef BUILD_LK
	static const struct jd_cmd unlock[] = {
		{ 0xdf, 3, { 0x91, 0x68, 0xf9 } },
		{ 0xde, 1, { 0x00 } },
		{ REGFLAG_END_OF_TABLE, 0, { 0 } },
	};
	unsigned int max_return = 0x00023700;   /* set maximum return packet size: 2 bytes */
	unsigned char id[2] = { 0, 0 };

	pin_out(GPIO_LCD_RST, 0);
	MDELAY(1);
	pin_out(GPIO_LCD_RST, 1);
	MDELAY(20);
	send_cmds(unlock);
	lcm_util.dsi_set_cmdq(&max_return, 1, 1);
	lcm_util.dsi_dcs_read_lcm_reg_v2(0xb2, id, 2);
	g_jd9168s_glass_f = (id[1] == 'F');
	dprintf(CRITICAL, "[LK/LCM] jd9168s glass id %02x %02x\n", id[0], id[1]);
#endif
	return 1;   /* the stock code accepts every glass */
}

static void lcm_init_lcm(void)
{
#ifdef BUILD_LK
	lcm_compare_id();
	pin_out(GPIO_LCD_RST, 1);
	MDELAY(10);
	pin_out(GPIO_LCD_RST, 0);
	MDELAY(10);
	pin_out(GPIO_LCD_RST, 1);
	MDELAY(10);
	jd_init[GLASS_ENTRY].cmd = (g_jd9168s_glass_f == 1) ? 0xb2 : 0xb3;
	send_cmds(jd_init);
#endif
}

static void lcm_suspend(void)
{
	send_cmds(jd_sleep);
#ifdef BUILD_LK
	pin_out(GPIO_LCD_RST, 0);
	MDELAY(10);
#endif
}

static void lcm_resume(void)
{
	lcm_init_lcm();
}

static void lcm_power_noop(void)
{
	/* the stock suspend_power and resume_power do nothing; the kernel owns the rails */
}

LCM_DRIVER jd9168s_960x720_dsi_vdo_m8695_bw01_lcm_drv = {
	/* Reported to the kernel in videolfb; must stay the stock LK's name. */
	.name = "jd9168s_960x720_dsi_vdo_m8695_bw01_drv",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init_lcm,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
	.compare_id = lcm_compare_id,
	.init_power = lcm_init_power,
	.suspend_power = lcm_power_noop,
	.resume_power = lcm_power_noop,
};
