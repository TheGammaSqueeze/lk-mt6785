/*
 * block_bt.c - Bluetooth on the Konkr Pocket Block: the MT6768's own connectivity subsystem
 * (CONNSYS), brought up from the bootloader for GammaOS Pico's console to console link (local
 * netplay, file transfer).
 *
 * CONNSYS is a small MCU of its own beside the AP. Bringing it up takes, in order:
 *   1. its supplies from the MT6358: VCN18, VCN28 and VCN33 (3.3 V, the Bluetooth radio);
 *   2. the pins of the interface to its analog die put in their CONNSYS function;
 *   3. its power domain on (SPM) and the AP's bus to it opened (INFRACFG bus protection);
 *   4. its firmware images placed in a window of DRAM it reads through a remap register, before
 *      its CPU is let out of reset;
 *   5. then a byte pipe to it (BTIF), a framing protocol on that pipe (STP) and its control
 *      commands (WMT) to patch it and switch the Bluetooth function on, after which the pipe
 *      carries plain HCI.
 * This file holds the Block's part (supplies, pins, power domain, the CPU's start); steps 4 and 5
 * and everything after are console-neutral, in platform/common/pico/pico_bt.inc.
 *
 * Registers here are as the SoC's documentation and the vendor's GPL kernel drivers describe the
 * hardware; the code is GammaOS's own.
 */
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <string.h>
#include <stdarg.h>
#include <printf.h>
#include <debug.h>

extern void udelay(unsigned long usec);
extern void mdelay(unsigned long msec);
extern unsigned int gpt4_get_current_tick(void);
extern signed int pwrap_read(unsigned int adr, unsigned int *rdata);
extern signed int pwrap_write(unsigned int adr, unsigned int wdata);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern void fastboot_register(const char *prefix, void (*handle)(const char *arg, void *data, unsigned sz),
			      unsigned allowed_when_security_on, unsigned forbidden_when_lock_on);

#define R32(a)		(*(volatile unsigned int *)(addr_t)(a))
#define GPT_US(t)	((t) / 13u)		/* the 13 MHz GPT */

/* ---- the MT6358 supplies ---- */
#define PMIC_VCN18_CON0		0x1c58		/* bit 0 on */
#define PMIC_VCN18_CON1		0x1c66		/* bit 15: the regulator reports it is on */
#define PMIC_VCN33_CON0_BT	0x1d1c		/* bit 0 on, the Bluetooth user of VCN33 */
#define PMIC_VCN33_CON1		0x1d2c		/* bit 15: on */
#define PMIC_VCN33_ANA_CON0	0x1e28		/* bits 9:8 voltage: 1 = 3.3 V */
#define PMIC_VCN28_CON0		0x1d88		/* bit 0 on */
#define PMIC_VCN28_CON1		0x1d96		/* bit 15: on */

static unsigned int pmic_rd(unsigned int reg)
{
	unsigned int v = 0;
	pwrap_read(reg, &v);
	return v & 0xffffu;
}
static void pmic_set(unsigned int reg, unsigned int mask, unsigned int val)
{
	pwrap_write(reg, (pmic_rd(reg) & ~mask) | (val & mask));
}

static void bt_supplies(int on)
{
	if (on) {
		pmic_set(PMIC_VCN18_CON0, 1, 1);
		pmic_set(PMIC_VCN28_CON0, 1, 1);
		pmic_set(PMIC_VCN33_ANA_CON0, 3u << 8, 1u << 8);
		pmic_set(PMIC_VCN33_CON0_BT, 1, 1);
		udelay(300);
	} else {
		pmic_set(PMIC_VCN33_CON0_BT, 1, 0);
		pmic_set(PMIC_VCN28_CON0, 1, 0);
		pmic_set(PMIC_VCN18_CON0, 1, 0);
	}
}

/* ---- the 26 MHz clock to the radio: the MT6358's XO buffer 2 (XO_WCN) ----
 * DCXO_CW00 bits 4:3 pick how the buffer is driven (0 by software, its enable then bit 5; 1..3
 * by hardware requests), and the PMIC wrapper's DCXO_ENABLE bit 1 lets CONNSYS's own clock
 * request through. Forcing it on is what Android does when it must be sure of the clock. */
#define PMIC_DCXO_CW00		0x788
#define XO_WCN_MODE_MASK	(3u << 3)
#define XO_WCN_EN_M		(1u << 5)
#define PWRAP_DCXO_ENABLE	0x1000d190u
#define DCXO_CONN_ENABLE	(1u << 1)

static unsigned int s_xo_saved_cw00 = 0xffffffffu, s_xo_saved_dcxo;

static void bt_xo_force(int on)
{
	if (on) {
		if (s_xo_saved_cw00 == 0xffffffffu) {
			s_xo_saved_cw00 = pmic_rd(PMIC_DCXO_CW00);
			s_xo_saved_dcxo = R32(PWRAP_DCXO_ENABLE);
		}
		R32(PWRAP_DCXO_ENABLE) = s_xo_saved_dcxo & ~DCXO_CONN_ENABLE;
		pwrap_write(PMIC_DCXO_CW00, (s_xo_saved_cw00 & ~XO_WCN_MODE_MASK) | XO_WCN_EN_M);
		udelay(400);
	} else if (s_xo_saved_cw00 != 0xffffffffu) {
		pwrap_write(PMIC_DCXO_CW00, s_xo_saved_cw00);
		R32(PWRAP_DCXO_ENABLE) = s_xo_saved_dcxo;
		s_xo_saved_cw00 = 0xffffffffu;
	}
}

/* ---- the analog die interface pins: GPIO mode registers, CONNSYS function (mode 1) ---- */
#define GPIO_BASE		0x10005000u
#define GPIO_MODE_CONN_A	0x3f0		/* two pins: mode fields 0 and 1 */
#define GPIO_MODE_CONN_B	0x3e0		/* seven pins: mode fields 1..7 */

static void bt_pins(int on)
{
	unsigned int a = R32(GPIO_BASE + GPIO_MODE_CONN_A) & 0xffffff88u;
	unsigned int b = R32(GPIO_BASE + GPIO_MODE_CONN_B) & 0x8888888fu;
	R32(GPIO_BASE + GPIO_MODE_CONN_A) = on ? (a | 0x00000011u) : a;
	R32(GPIO_BASE + GPIO_MODE_CONN_B) = on ? (b | 0x11111110u) : b;
}

/* ---- the power domain and the bus to it ---- */
#define SPM_BASE		0x10006000u
#define SPM_POWERON_CONFIG	0x000		/* the project key in bits 31:16 lets writes through */
#define SPM_PROJECT_KEY		(0x0b16u << 16)
#define SPM_PWR_STATUS		0x180
#define SPM_PWR_STATUS_2ND	0x184
#define SPM_CONN_PWR_CON	0x32c
#define PWR_RST_B		(1u << 0)
#define PWR_ISO			(1u << 1)
#define PWR_ON			(1u << 2)
#define PWR_ON_2ND		(1u << 3)
#define PWR_CLK_DIS		(1u << 4)
#define CONN_STA		(1u << 1)	/* the domain's bit in both status words */

#define INFRACFG_BASE		0x10001000u
#define INFRA_PROT_CLR		0x2a4		/* bus protection, clear: bits 13 and 14 are CONNSYS's */
#define INFRA_PROT_SET		0x2a0
#define INFRA_PROT_STA1		0x228
#define INFRA_PROT1_CLR		0x2ac		/* the second set: bit 18 */
#define INFRA_PROT1_SET		0x2a8
#define INFRA_PROT1_STA1	0x258
#define CONN_PROT_A		(1u << 13)
#define CONN_PROT_B		(1u << 14)
#define CONN_PROT1		(1u << 18)
#define INFRA_CG2_SET		0x080		/* clock gates, one sets (gates the clock) */
#define INFRA_CG2_CLR		0x084		/* clock gates, one clears: */
#define INFRA_CG2_STA		0x090
#define INFRA_CG2_BTIF		(1u << 31)
#define INFRA_CG2_PMIC_CONN	(1u << 3)
#define INFRA_EMI_REMAP		0x380		/* bits 11:0: the DRAM 16 MB page CONNSYS sees at 0xF0000000 */
#define INFRA_PERI_REMAP	0x388		/* bits 27:16: the peripheral page it sees */

#define TOPRGU_BASE		0x10007000u
#define TOPRGU_SWSYSRST		0x018		/* key 0x88 in bits 31:24 */
#define RGU_KEY			(0x88u << 24)
#define RGU_CONN_CPU		(1u << 12)	/* the CONNSYS CPU held in reset */
#define RGU_CONN_SYS		(1u << 9)

#define CONN_TOP_MISC_OFF	0x180b1000u
#define CONN_IP_VER		(CONN_TOP_MISC_OFF + 0x10)
#define CONN_IP_VER_MT6768	0x10020501u
#define CONN_MCU_BASE		0x18002000u
#define CONN_HW_VER		(CONN_MCU_BASE + 0x0)
#define CONN_FW_VER		(CONN_MCU_BASE + 0x4)

volatile unsigned int g_bt_dbg_step;	/* where the power on got to (oem btpwr) */

/* wait for (reg & mask) == want, up to us microseconds; 0 when it got there */
static int bt_wait(unsigned int reg, unsigned int mask, unsigned int want, unsigned int us)
{
	unsigned int t0 = gpt4_get_current_tick(), w;
	while ((R32(reg) & mask) != want) {
		w = GPT_US(gpt4_get_current_tick() - t0);
		if (w > us)
			return -1;
		if (w > 200)		/* a longer wait: the CPU to the other threads meanwhile */
			thread_yield();
	}
	return 0;
}

static int bt_domain_on(void)
{
	unsigned int spm = SPM_BASE;

	g_bt_dbg_step = 1;
	R32(spm + SPM_POWERON_CONFIG) = (R32(spm + SPM_POWERON_CONFIG) & 0xffffu) | SPM_PROJECT_KEY | 1u;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_ON;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_ON_2ND;
	if (bt_wait(spm + SPM_PWR_STATUS, CONN_STA, CONN_STA, 10000) ||
	    bt_wait(spm + SPM_PWR_STATUS_2ND, CONN_STA, CONN_STA, 10000))
		return -1;
	g_bt_dbg_step = 2;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_CLK_DIS;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_ISO;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_RST_B;
	g_bt_dbg_step = 3;
	/* the bus: release the protection, the later steps first, as the vendor does */
	R32(INFRACFG_BASE + INFRA_PROT_CLR) = CONN_PROT_B;
	R32(INFRACFG_BASE + INFRA_PROT_CLR) = CONN_PROT_A;
	R32(INFRACFG_BASE + INFRA_PROT1_CLR) = CONN_PROT1;
	g_bt_dbg_step = 4;
	return 0;
}

static void bt_domain_off(void)
{
	unsigned int spm = SPM_BASE;

	R32(INFRACFG_BASE + INFRA_PROT_SET) = CONN_PROT_A;
	bt_wait(INFRACFG_BASE + INFRA_PROT_STA1, CONN_PROT_A, CONN_PROT_A, 10000);
	R32(INFRACFG_BASE + INFRA_PROT1_SET) = CONN_PROT1;
	bt_wait(INFRACFG_BASE + INFRA_PROT1_STA1, CONN_PROT1, CONN_PROT1, 10000);
	R32(INFRACFG_BASE + INFRA_PROT_SET) = CONN_PROT_B;
	bt_wait(INFRACFG_BASE + INFRA_PROT_STA1, CONN_PROT_B, CONN_PROT_B, 10000);
	R32(spm + SPM_POWERON_CONFIG) = (R32(spm + SPM_POWERON_CONFIG) & 0xffffu) | SPM_PROJECT_KEY | 1u;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_ISO;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_CLK_DIS;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_RST_B;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_ON;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_ON_2ND;
}

static int s_bt_powered;
static unsigned int s_cg_was_gated;	/* the clock gates CONNSYS's power on opened */

/* steps 1 to 3, with the CONNSYS CPU kept in reset; 0 when the subsystem answers with its id */
int block_bt_power_on(void)
{
	if (s_bt_powered)
		return 0;
	bt_supplies(1);
	bt_pins(1);
	/* the clocks of the byte pipe and of the PMIC's link for CONNSYS: write one to ungate; the
	 * ones that were gated are gated again at power off */
	s_cg_was_gated = R32(INFRACFG_BASE + INFRA_CG2_STA) & (INFRA_CG2_BTIF | INFRA_CG2_PMIC_CONN);
	R32(INFRACFG_BASE + INFRA_CG2_CLR) = INFRA_CG2_BTIF | INFRA_CG2_PMIC_CONN;
	udelay(150);
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) | RGU_CONN_CPU) | RGU_KEY;
	if (bt_domain_on() != 0)
		return -1;
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) & ~RGU_CONN_SYS) | RGU_KEY;
	udelay(10);
	g_bt_dbg_step = 5;
	if (bt_wait(CONN_IP_VER, 0xffffffffu, CONN_IP_VER_MT6768, 200000) != 0)
		return -2;
	g_bt_dbg_step = 6;
	s_bt_powered = 1;
	return 0;
}

void block_bt_power_off(void)
{
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) | RGU_CONN_CPU) | RGU_KEY;
	bt_domain_off();
	bt_pins(0);
	bt_supplies(0);
	if (s_cg_was_gated)			/* the clocks as they were before */
		R32(INFRACFG_BASE + INFRA_CG2_SET) = s_cg_was_gated;
	bt_xo_force(0);
	s_bt_powered = 0;
}


/* ---- the hooks pico_bt.inc runs ---- */
#include <platform/boot_mode.h>
#include <arch/arm/mmu.h>
#include "../../emu/gba/fat_ro.h"
#include <platform/pico_bt.h>		/* BT_EMI_PA, BT_EMI_SIZE */
#define PICO_BT_BTIF_BASE	0x1100c000u
#define PICO_BT_IRQ_ID		(133 + 32)	/* SPI 133, active low */
#include <platform/mt_irq.h>
#include <mt_gic.h>
#define PICO_BT_CHIP_ID		0x6768
#define PICO_BT_MODEL		2		/* 1 = the Konkr Pocket Advance */
#define PICO_BT_FW_RAM_MCU	"soc1_0_ram_mcu_1a_1_hdr.bin"
#define PICO_BT_FW_RAM_BT	"soc1_0_ram_bt_1a_1_hdr.bin"
#define PICO_BT_FW_RAM_WIFI	"soc1_0_ram_wifi_1a_1_hdr.bin"
#define PICO_BT_FW_PATCH_MCU	"soc1_0_patch_mcu_1a_1_hdr.bin"

static void bt_log(const char *fmt, ...);

static int pico_bt_plat_powered(void) { return s_bt_powered; }
static void pico_bt_plat_off(void) { block_bt_power_off(); }

static int pico_bt_plat_start(void)
{
	unsigned int r;

	/* CONNSYS's view of the window and of the peripherals */
	r = R32(INFRACFG_BASE + INFRA_EMI_REMAP);
	R32(INFRACFG_BASE + INFRA_EMI_REMAP) = (r & ~0xfffu) | (BT_EMI_PA >> 24);
	r = R32(INFRACFG_BASE + INFRA_PERI_REMAP);
	R32(INFRACFG_BASE + INFRA_PERI_REMAP) = (r & 0xf000ffffu) | (0x10u << 16);

	if (block_bt_power_on() != 0) {
		bt_log("power on failed, step %u", g_bt_dbg_step);
		return 1;
	}
	bt_log("ip %08x hw %04x", R32(CONN_IP_VER), R32(CONN_HW_VER) & 0xffffu);
	R32(CONN_MCU_BASE + 0x150) &= ~(1u << 21);		/* DRAM access in hardware mode */
	R32(0x180c1000u + 0x600) |= 1u << 23;			/* the restore-done flag */
	r = R32(0x180b3000u + 0x34);				/* the AFE PLL setting */
	R32(0x180b3000u + 0x34) = (r & 0xffe7ffffu) | 0x80000u;
	/* the CPU out of reset; its ROM reports 0x1D1E when it is up */
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) & ~RGU_CONN_CPU) | RGU_KEY;
	if (bt_wait(CONN_MCU_BASE + 0x600, 0xffffu, 0x1d1eu, 300000) != 0) {
		bt_log("cpu not up: %08x pc %08x", R32(CONN_MCU_BASE + 0x600), R32(0x18007104u));
		return 2;
	}
	bt_log("cpu up");
	return 0;
}

#include "../common/pico/pico_bt.inc"

/* power off, before the kernel and the console's own power off: nothing to do when it is off */
void pico_bt_power_off_if_on(void)
{
	if (s_bt_powered)
		pico_bt_off();
}

/* ---- the bootloader channel: this console's diagnostics ---- */

/* the bootloader channel carries about 60 characters a line */
static void bt_report(void)
{
	snprintf(s_line, sizeof s_line, "vcn18 %u/%u vcn28 %u/%u vcn33 %u/%u sel %u",
		 pmic_rd(PMIC_VCN18_CON0) & 1, pmic_rd(PMIC_VCN18_CON1) >> 15,
		 pmic_rd(PMIC_VCN28_CON0) & 1, pmic_rd(PMIC_VCN28_CON1) >> 15,
		 pmic_rd(PMIC_VCN33_CON0_BT) & 1, pmic_rd(PMIC_VCN33_CON1) >> 15,
		 (pmic_rd(PMIC_VCN33_ANA_CON0) >> 8) & 3);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "pwr_con %08x sta %08x %08x", R32(SPM_BASE + SPM_CONN_PWR_CON),
		 R32(SPM_BASE + SPM_PWR_STATUS), R32(SPM_BASE + SPM_PWR_STATUS_2ND));
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "prot %08x %08x rgu %08x", R32(INFRACFG_BASE + INFRA_PROT_STA1),
		 R32(INFRACFG_BASE + INFRA_PROT1_STA1), R32(TOPRGU_BASE + TOPRGU_SWSYSRST));
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "step %u powered %d", g_bt_dbg_step, s_bt_powered);
	fastboot_info(s_line);
	if (s_bt_powered) {
		snprintf(s_line, sizeof s_line, "ip %08x hw %04x fw %04x", R32(CONN_IP_VER),
			 R32(CONN_HW_VER) & 0xffffu, R32(CONN_FW_VER) & 0xffffu);
		fastboot_info(s_line);
	}
}

/* oem btpwr[:0] - power CONNSYS on (or off with :0) and report each step's state */
static void cmd_btpwr(const char *arg, void *data, unsigned sz)
{
	int rc = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == '0')
		pico_bt_off();
	else
		rc = block_bt_power_on();
	snprintf(s_line, sizeof s_line, "bt power %s rc=%d", *arg == '0' ? "off" : "on", rc);
	fastboot_info(s_line);
	bt_report();
	fastboot_okay("");
}

/* oem btrd:ADDR[:N] - CONNSYS registers (0x18000000..0x18ffffff), hex, read only, the domain on */
static void cmd_btrd(const char *arg, void *data, unsigned sz)
{
	unsigned int a = 0, n = 1, i;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while ((*arg >= '0' && *arg <= '9') || (*arg >= 'a' && *arg <= 'f') || (*arg >= 'A' && *arg <= 'F')) {
		a = (a << 4) | (unsigned)(*arg <= '9' ? *arg - '0' : (*arg | 0x20) - 'a' + 10);
		arg++;
	}
	if (*arg == ':') {
		n = 0;
		for (arg++; *arg >= '0' && *arg <= '9'; arg++) n = n * 10 + (unsigned)(*arg - '0');
	}
	if (n < 1) n = 1;
	if (n > 16) n = 16;
	a &= ~3u;
	if (!s_bt_powered || a < 0x18000000u || a + n * 4 > 0x19000000u) {
		fastboot_fail("btrd: CONNSYS off or address outside 0x18000000..0x18ffffff");
		return;
	}
	for (i = 0; i < n; i += 4) {
		unsigned int k, len = (unsigned)snprintf(s_line, sizeof s_line, "%08x:", a + i * 4);
		for (k = i; k < n && k < i + 4; k++)
			len += (unsigned)snprintf(s_line + len, sizeof s_line - len, " %08x", R32(a + k * 4));
		fastboot_info(s_line);
	}
	fastboot_okay("");
}

/* oem btxo:1|0 - the radio's 26 MHz buffer forced on, or back as it was; shows CW00 */
static void cmd_btxo(const char *arg, void *data, unsigned sz)
{
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == '1' || *arg == '0')
		bt_xo_force(*arg == '1');
	snprintf(s_line, sizeof s_line, "cw00 %04x dcxo_en %08x", pmic_rd(PMIC_DCXO_CW00),
		 R32(PWRAP_DCXO_ENABLE));
	fastboot_info(s_line);
	fastboot_okay("");
}

void block_bt_register_cmds(void)
{
	pico_bt_register_cmds();
	fastboot_register("oem btxo", cmd_btxo, 1, 0);
	fastboot_register("oem btpwr", cmd_btpwr, 1, 0);
	fastboot_register("oem btrd:", cmd_btrd, 1, 0);
}
