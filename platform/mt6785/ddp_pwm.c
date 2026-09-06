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

#define LOG_TAG "PWM"
#include <string.h>
#include <platform/sync_write.h>
#include <cust_leds.h>
#include <debug.h>
#include <platform/ddp_reg.h>
#include <platform/ddp_pwm.h>
#include "platform/ddp_info.h"
#include <platform/disp_drv_log.h>


extern int ddp_enable_module_clock(DISP_MODULE_ENUM module);

/* DISP_PWM_MUX setting */
#ifndef CLK_CFG_8
#define CLK_CFG_8 0x100000A0
#endif

#ifndef CLK_CFG_8_SET
#define CLK_CFG_8_SET 0x100000A4
#endif

#ifndef CLK_CFG_8_CLR
#define CLK_CFG_8_CLR 0x100000A8
#endif

#define CLK_CFG_UPDATE1 0x10000008
#define DISP_PWM_MUX_CK_UPDATE	(1)

#define PWM_MSG(fmt, arg...) DISPMSG("[PWM] " fmt "\n", ##arg)
#define PWM_DBG(fmt, arg...) DISPDBG("[PWM] " fmt "\n", ##arg)

#define PWM_REG_SET(reg, value) do { \
        mt65xx_reg_sync_writel(value, (volatile unsigned int*)(reg)); \
        PWM_DBG("set reg[0x%08x] = 0x%08x", (unsigned int)reg, (unsigned int)value); \
    } while (0)

#define PWM_REG_GET(reg) *((volatile unsigned int*)reg)

#define PWM_DEFAULT_DIV_VALUE 0x0

#define pwm_get_reg_base(id) ((id) == DISP_PWM0 ? DISPSYS_PWM0_BASE : DISPSYS_PWM0_BASE)

#define index_of_pwm(id) ((id == DISP_PWM0) ? 0 : 1)

static int g_pwm_inited = 0;
static disp_pwm_id_t g_pwm_main_id = DISP_PWM0;

/* AYANEO motion-blur strobe: when non-zero, disp_pwm_set_backlight writes this into the PWM
 * clock divider (CON_0), dropping the dimming carrier to ~frame rate so the backlight strobes
 * instead of dimming uniformly = shorter sample-and-hold = less perceived motion blur. 0 = off
 * (normal ~kHz dimming). 256 gives ~98Hz on this panel (flicker-free, real blur reduction). The
 * user's brightness rides on the duty (CON_1), so brightness and strobe cooperate. */
static unsigned int g_strobe_div = 0;
void disp_pwm_strobe_mode(int on)
{
	g_strobe_div = on ? 256u : 0u;
	/* Apply the divider to CON_0 immediately. The brightness path caches same-level calls and
	 * skips the PWM write, so a menu toggle would otherwise not take effect until the next
	 * brightness change. Guarded until the PWM is inited: the early-boot settings load just sets
	 * the variable, and the first post-init backlight set then applies it. CON_1 (duty) is left
	 * as-is so the current brightness is preserved. */
	if (g_pwm_inited) {
		unsigned int reg_base = pwm_get_reg_base(g_pwm_main_id);
		PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
		PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, g_strobe_div << 16);
		PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 1);
		PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
	}
}


void disp_pwm_clkmux_update(void)
{
	unsigned int regsrc = PWM_REG_GET(CLK_CFG_UPDATE1);

	regsrc = regsrc | (0x1 << DISP_PWM_MUX_CK_UPDATE);
	/* write 1 to update the change of pwm clock source selection */
	PWM_REG_SET(CLK_CFG_UPDATE1, regsrc);
}


void disp_pwm_init(disp_pwm_id_t id)
{
	struct cust_mt65xx_led *cust_led_list;
	struct cust_mt65xx_led *cust;
	struct PWM_config *config_data;
	unsigned int pwm_div;
	unsigned int reg_base = pwm_get_reg_base(id);

	if (g_pwm_inited & id)
		return;

	if (id & DISP_PWM0)
		ddp_enable_module_clock(DISP_MODULE_PWM0);
	if (id & DISP_PWM1)
		ASSERT(0);

	pwm_div = PWM_DEFAULT_DIV_VALUE;
	cust_led_list = get_cust_led_list();
	if (cust_led_list) {
		/* WARNING: may overflow if MT65XX_LED_TYPE_LCD not configured properly */
		cust = &cust_led_list[MT65XX_LED_TYPE_LCD];
		if ((strcmp(cust->name,"lcd-backlight") == 0) && (cust->mode == MT65XX_LED_MODE_CUST_BLS_PWM)) {
			config_data = &cust->config_data;
			if (config_data->clock_source >= 0 && config_data->clock_source <= 4) {
				unsigned int reg_val = PWM_REG_GET(CLK_CFG_8);
				PWM_REG_SET(CLK_CFG_8_CLR, 0x7);
				PWM_REG_SET(CLK_CFG_8_SET, (0x7 & (config_data->clock_source << 0)));
				PWM_DBG("disp_pwm_init : CLK_CFG_8 0x%x => 0x%x", reg_val, PWM_REG_GET(CLK_CFG_8));
			}
			/*
			After config clock source register, pwm hardware doesn't switch to new setting yet.
			Api function "disp_pwm_clkmux_update" need to be called for trigger pwm hardware
			to accept new clock source setting.
			*/
			disp_pwm_clkmux_update();
			/* Some backlight chip/PMIC(e.g. MT6332) only accept slower clock */
			pwm_div = (config_data->div == 0) ? PWM_DEFAULT_DIV_VALUE : config_data->div;
			pwm_div &= 0x3FF;
			PWM_MSG("disp_pwm_init : PWM config data (%d,%d)", config_data->clock_source, config_data->div);
		}
	}

	PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, pwm_div << 16);

	PWM_REG_SET(reg_base + DISP_PWM_CON_1_OFF, 1023); /* 1024 levels */

	g_pwm_inited |= id;
}


disp_pwm_id_t disp_pwm_get_main(void)
{
	return g_pwm_main_id;
}


int disp_pwm_is_enabled(disp_pwm_id_t id)
{
	unsigned int reg_base = pwm_get_reg_base(id);
	return (PWM_REG_GET(reg_base + DISP_PWM_EN_OFF) & 0x1);
}


static void disp_pwm_set_enabled(disp_pwm_id_t id, int enabled)
{
	unsigned int reg_base = pwm_get_reg_base(id);
	if (enabled) {
		if (!disp_pwm_is_enabled(id)) {
			PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x1);
			PWM_MSG("disp_pwm_set_enabled: PWN_EN = 0x1");
		}
	} else {
		PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x0);
	}
}


/* For backward compatible */
int disp_bls_set_backlight(int level_256)
{
	int level_1024 = 0;

	if (level_256 <= 0)
		level_1024 = 0;
	else if (level_256 >= 255)
		level_1024 = 1023;
	else {
		level_1024 = (level_256 << 2) + 2;
	}

	return disp_pwm_set_backlight(disp_pwm_get_main(), level_1024);
}


static int disp_pwm_level_remap(disp_pwm_id_t id, int level_1024)
{
	return level_1024;
}


int disp_pwm_set_backlight(disp_pwm_id_t id, int level_1024)
{
	unsigned int reg_base;
	unsigned int offset;

	if ((DISP_PWM_ALL & id) == 0) {
		PWM_MSG("[ERROR] disp_pwm_set_backlight: invalid PWM ID = 0x%x", id);
		return -1;
	}

	disp_pwm_init(id);

	PWM_MSG("disp_pwm_set_backlight(id = 0x%x, level_1024 = %d)", id, level_1024);

	reg_base = pwm_get_reg_base(id);

	level_1024 = disp_pwm_level_remap(id, level_1024);

	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
	PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, g_strobe_div << 16);  /* strobe divider (0 = normal dimming) */
	PWM_REG_SET(reg_base + DISP_PWM_CON_1_OFF, (level_1024 << 16) | 0x3ff);

	if (level_1024 > 0) {
		disp_pwm_set_enabled(id, 1);
	} else {
		disp_pwm_set_enabled(id, 0); /* To save power */
	}

	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 1);
	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);

	for (offset = 0x0; offset <= 0x28; offset += 4) {
		PWM_DBG("reg[0x%08x] = 0x%08x", (reg_base + offset), PWM_REG_GET(reg_base + offset));
	}

	return 0;
}


/* AYANEO backlight strobe (motion-blur / response-lag reduction).
 *
 * The DISP_PWM dimming carrier freq = src_clk / (div+1) / 1024. Stock div=0 gives a
 * ~kHz carrier, so a reduced duty just DIMS uniformly. Cranking the divider drops the
 * carrier toward the frame rate, at which point a duty < full makes the backlight go
 * DARK for part of each ~frame = a strobe that shortens the sample-and-hold time and
 * cuts perceived motion blur. div sweeps the strobe rate, duty the ON fraction (of 1024).
 *
 * Two modes:
 *  - disp_pwm_strobe(): FREE-RUNNING carrier. Simple but drifts against the panel scan,
 *    so at a strobe rate != frame rate the eye sums two frames = a double-image.
 *  - disp_pwm_strobe_locked() + disp_pwm_strobe_tick(): the tick restarts the PWM counter
 *    once per frame at the FRAME_DONE point (primary_display_config_input), so EXACTLY one
 *    pulse fires per frame, phase-locked to the panel. That removes the double-image while
 *    keeping the persistence cut. div sets the period (~1 frame), duty the ON fraction.
 * div=0 disables the strobe and restores the normal dimming carrier at the given duty. */
static volatile int          g_strobe_locked;   /* 1 = re-align the pulse every frame */

void disp_pwm_strobe(unsigned int div, unsigned int duty)
{
	unsigned int reg_base = pwm_get_reg_base(g_pwm_main_id);

	disp_pwm_init(g_pwm_main_id);
	g_strobe_locked = 0;   /* free-running: the per-frame tick does nothing */
	if (div > 0x3ff)  div = 0x3ff;
	if (duty > 0x3ff) duty = 0x3ff;

	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
	PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, div << 16);
	PWM_REG_SET(reg_base + DISP_PWM_CON_1_OFF, (duty << 16) | 0x3ff);
	if (duty > 0)
		PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x1);
	else
		PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x0);
	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 1);
	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
}

/* Vsync-locked strobe: same waveform, but re-latched each frame by disp_pwm_strobe_tick().
 * div=0 (or duty=0) disables the lock and restores the normal carrier at `duty` brightness. */
void disp_pwm_strobe_locked(unsigned int div, unsigned int duty)
{
	if (div == 0 || duty == 0) {
		disp_pwm_strobe(0, duty ? duty : 1023);   /* clears g_strobe_locked, restores carrier */
		return;
	}
	disp_pwm_strobe(div, duty);   /* apply the waveform (clears lock)... */
	g_strobe_locked = 1;          /* ...then arm the per-frame re-lock */
}

/* Called once per frame from the FRAME_DONE point in primary_display_config_input. Restart
 * the PWM counter (EN off->on) so the single pulse re-aligns to THIS frame = phase lock.
 * Cheap (2 register writes); no-op unless a locked strobe is active. */
void disp_pwm_strobe_tick(void)
{
	unsigned int reg_base;

	if (!g_strobe_locked)
		return;
	reg_base = pwm_get_reg_base(g_pwm_main_id);
	PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x0);
	PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x1);
}


