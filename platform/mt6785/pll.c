/* Copyright Statement:
 *
 * This software/firmware and related documentation ("MediaTek Software") are
 * protected under relevant copyright laws. The information contained herein is
 * confidential and proprietary to MediaTek Inc. and/or its licensors. Without
 * the prior written permission of MediaTek inc. and/or its licensors, any
 * reproduction, modification, use or disclosure of MediaTek Software, and
 * information contained herein, in whole or in part, shall be strictly
 * prohibited.
 *
 * MediaTek Inc. (C) 2015. All rights reserved.
 *
 * BY OPENING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
 * THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
 * RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER
 * ON AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR
 * NONINFRINGEMENT. NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH
 * RESPECT TO THE SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY,
 * INCORPORATED IN, OR SUPPLIED WITH THE MEDIATEK SOFTWARE, AND RECEIVER AGREES
 * TO LOOK ONLY TO SUCH THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO.
 * RECEIVER EXPRESSLY ACKNOWLEDGES THAT IT IS RECEIVER'S SOLE RESPONSIBILITY TO
 * OBTAIN FROM ANY THIRD PARTY ALL PROPER LICENSES CONTAINED IN MEDIATEK
 * SOFTWARE. MEDIATEK SHALL ALSO NOT BE RESPONSIBLE FOR ANY MEDIATEK SOFTWARE
 * RELEASES MADE TO RECEIVER'S SPECIFICATION OR TO CONFORM TO A PARTICULAR
 * STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S
 * ENTIRE AND CUMULATIVE LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE
 * RELEASED HEREUNDER WILL BE, AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE
 * MEDIATEK SOFTWARE AT ISSUE, OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE
 * CHARGE PAID BY RECEIVER TO MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.
 *
 * The following software/firmware and/or related documentation ("MediaTek
 * Software") have been modified by MediaTek Inc. All revisions are subject to
 * any receiver's applicable license agreements with MediaTek Inc.
 */

#include <platform/mt_typedefs.h>


#define APMIXED_BASE      (0x1000C000)

#define TVDPLL_CON0             (APMIXED_BASE + 0x270)
#define TVDPLL_PWR_CON0         (APMIXED_BASE + 0x27C)

#define APLL1_CON0              (APMIXED_BASE + 0x2A0)
#define APLL1_PWR_CON0          (APMIXED_BASE + 0x2B0)

#define APLL2_CON0              (APMIXED_BASE + 0x2B4)
#define APLL2_PWR_CON0          (APMIXED_BASE + 0x2C4)

#define PLL_PWR_CON0_PWR_ON_BIT 0
#define PLL_PWR_CON0_ISO_EN_BIT 1
#define PLL_CON0_EN_BIT 0

extern void cmdline_append(const char* append_string);

static void clk_bootargs(void)
{
	cmdline_append("clk_ignore_unused");
}

void mt_pll_turn_off(void)
{
	/*unsigned int temp;*/
	clk_bootargs();
#if 0
    /***********************
      * xPLL Frequency Disable
      ************************/
	temp = DRV_Reg32(TVDPLL_CON0);
	DRV_WriteReg32(TVDPLL_CON0, temp & ~(1<<PLL_CON0_EN_BIT));

	temp = DRV_Reg32(APLL1_CON0);
	DRV_WriteReg32(APLL1_CON0, temp & ~(1<<PLL_CON0_EN_BIT));

	temp = DRV_Reg32(APLL2_CON0);
	DRV_WriteReg32(APLL2_CON0, temp & ~(1<<PLL_CON0_EN_BIT));

    /******************
    * xPLL PWR ISO Enable
    *******************/
	temp = DRV_Reg32(TVDPLL_PWR_CON0);
	DRV_WriteReg32(TVDPLL_PWR_CON0, temp | (1<<PLL_PWR_CON0_ISO_EN_BIT));

	temp = DRV_Reg32(APLL1_PWR_CON0);
	DRV_WriteReg32(APLL1_PWR_CON0, temp | (1<<PLL_PWR_CON0_ISO_EN_BIT));

	temp = DRV_Reg32(APLL2_PWR_CON0);
	DRV_WriteReg32(APLL2_PWR_CON0, temp | (1<<PLL_PWR_CON0_ISO_EN_BIT));

    /*************
    * xPLL PWR OFF
    **************/
	temp = DRV_Reg32(TVDPLL_PWR_CON0);
	DRV_WriteReg32(TVDPLL_PWR_CON0, temp & ~(1<<PLL_PWR_CON0_PWR_ON_BIT));

	temp = DRV_Reg32(APLL1_PWR_CON0);
	DRV_WriteReg32(APLL1_PWR_CON0, temp & ~(1<<PLL_PWR_CON0_PWR_ON_BIT));

	temp = DRV_Reg32(APLL2_PWR_CON0);
	DRV_WriteReg32(APLL2_PWR_CON0, temp & ~(1<<PLL_PWR_CON0_PWR_ON_BIT));
#else
	clk_bootargs();
#endif
}


/*
 * AYANEO: read the ARM PLL output frequency in MHz. MTK PLL formula:
 * Fout = 26 MHz * PCW / 2^14 / 2^POSDIV, with PCW in CON1[21:0] and POSDIV in
 * CON1[26:24]. Used by the GammaOS Pico menu to display the CPU clock. This is
 * the PLL output; the cores may run at a further integer divide of it.
 */
unsigned int ayaneo_get_cpu_mhz(void)
{
	/* ARMPLL_CON1 = APMIXED_BASE(0x1000C000) + 0x204 */
	unsigned int con1 = *((volatile unsigned int *)(0x1000C000u + 0x204u));
	unsigned int pcw = con1 & 0x3FFFFF;
	unsigned int posdiv = (con1 >> 24) & 0x7;
	unsigned long long f = 26000000ull * pcw;

	f >>= 14;
	f >>= posdiv;
	return (unsigned int)(f / 1000000ull);
}

/*
 * AYANEO: set the ARM PLL output frequency (MHz) by rewriting CON1's PCW and
 * pulsing the PCW_CHG bit (bit 31) to relock the PLL, keeping the current
 * POSDIV. This changes the clock the CPU cores may be running on, so the ~20 us
 * relock can briefly disturb the CPU; it is intentionally NOT persisted, so a
 * bad value is cleared by a power cycle. Clamped to a sane range.
 */
void ayaneo_set_cpu_mhz(unsigned int mhz)
{
	volatile unsigned int *con1 = (volatile unsigned int *)(0x1000C000u + 0x204u);
	extern void udelay(unsigned long usec);
	unsigned int c = *con1;
	unsigned int posdiv = (c >> 24) & 0x7;
	unsigned long long pcw;

	if (mhz < 400) mhz = 400;
	if (mhz > 2100) mhz = 2100;
	pcw = ((unsigned long long)mhz * 1000000ull) << 14;	/* * 2^14 */
	pcw <<= posdiv;						/* * 2^POSDIV */
	pcw /= 26000000ull;					/* / 26 MHz */
	if (pcw > 0x3FFFFF) pcw = 0x3FFFFF;

	c = c & ~0x003FFFFFu & ~0x80000000u;	/* clear old PCW and CHG */
	c |= (unsigned int)pcw;
	*con1 = c;				/* new PCW, CHG=0 */
	*con1 = c | 0x80000000u;		/* CHG rising edge -> relock */
	udelay(20);				/* PLL relock time */
}
