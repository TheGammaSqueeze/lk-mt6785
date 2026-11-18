/*
 * kpa_bt.c - Bluetooth on the Konkr Pocket Advance: the MT6785's connectivity subsystem
 * (CONNSYS), the same family as the Konkr Pocket Block's, brought up from the bootloader for
 * GammaOS Pico's console to console link (local netplay, file transfer).
 *
 * This file holds the console's part: the MT6359 supplies (VCN18, VCN33_1 at 3.3 V for the
 * radio, VCN33_2 at 2.8 V), the analog die interface pins, the clocks, the power domain and the
 * bus to it, the window of DRAM CONNSYS reads its firmware from, and the steps around letting its
 * CPU out of reset. Everything after (the byte pipe, its framing, the control commands, HCI, LE)
 * is console-neutral, in platform/common/pico/pico_bt.inc. Nothing here runs at boot: Bluetooth
 * is brought up only when a Pico screen asks for it.
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

/* ---- the MT6359 supplies ---- */
#define PMIC_VCN18_CON0		0x1c2a		/* bit 0 on */
#define PMIC_VCN18_MON		0x1c2c		/* bit 0: the regulator reports it is on */
#define PMIC_VCN33_1_CON0	0x1bd8		/* bit 0 on: the Bluetooth user of VCN33_1 */
#define PMIC_VCN33_1_MON	0x1bda
#define PMIC_VCN33_1_ANA_CON0	0x1f98		/* bits 11:8 voltage: 13 = 3.3 V */
#define PMIC_VCN33_2_CON0	0x1c08		/* bit 0 on */
#define PMIC_VCN33_2_MON	0x1c0a
#define PMIC_VCN33_2_ANA_CON0	0x1f9c		/* bits 11:8 voltage: 9 = 2.8 V */
#define VOSEL_3V3		13u
#define VOSEL_2V8		9u

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
		pmic_set(PMIC_VCN33_1_ANA_CON0, 0xfu << 8, VOSEL_3V3 << 8);
		pmic_set(PMIC_VCN33_1_CON0, 1, 1);
		pmic_set(PMIC_VCN33_2_ANA_CON0, 0xfu << 8, VOSEL_2V8 << 8);
		pmic_set(PMIC_VCN33_2_CON0, 1, 1);
		udelay(300);
	} else {
		pmic_set(PMIC_VCN33_2_CON0, 1, 0);
		pmic_set(PMIC_VCN33_1_CON0, 1, 0);
		udelay(300);
		pmic_set(PMIC_VCN18_CON0, 1, 0);
	}
}

/* ---- the analog die interface pins: CONNSYS function, 2 mA drive ---- */
#define GPIO_BASE		0x10005000u
#define GPIO_MODE_CONN_A	0x430		/* mode fields 4..7 */
#define GPIO_MODE_CONN_B	0x440		/* mode fields 0..4 */
#define CONN_PIN_DRIVE		0x11ea0000u	/* bits 26:3 the drive strengths, 0 = 2 mA */

static void bt_pins(int on)
{
	unsigned int a = R32(GPIO_BASE + GPIO_MODE_CONN_A) & 0x0000ffffu;
	unsigned int b = R32(GPIO_BASE + GPIO_MODE_CONN_B) & 0xfff00000u;
	R32(GPIO_BASE + GPIO_MODE_CONN_A) = on ? (a | 0x11110000u) : a;
	R32(GPIO_BASE + GPIO_MODE_CONN_B) = on ? (b | 0x00011111u) : b;
	if (on)
		R32(CONN_PIN_DRIVE) = R32(CONN_PIN_DRIVE) & 0xff000007u;
}

/* ---- the power domain and the bus to it ---- */
#define SPM_BASE		0x10006000u
#define SPM_POWERON_CONFIG	0x000		/* the project key in bits 31:16 lets writes through */
#define SPM_PROJECT_KEY		(0x0b16u << 16)
#define SPM_PWR_STATUS		0x160
#define SPM_PWR_STATUS_2ND	0x164
#define SPM_CONN_PWR_CON	0x320
#define PWR_RST_B		(1u << 0)
#define PWR_ISO			(1u << 1)
#define PWR_ON			(1u << 2)
#define PWR_ON_2ND		(1u << 3)
#define PWR_CLK_DIS		(1u << 4)
#define CONN_STA		(1u << 1)	/* the domain's bit in both status words */

#define INFRACFG_BASE		0x10001000u
#define INFRA_PROT_SET		0x2a0		/* bus protection, first set */
#define INFRA_PROT_CLR		0x2a4
#define INFRA_PROT_STA1		0x228
#define INFRA_PROT1_SET		0x2a8		/* second set */
#define INFRA_PROT1_CLR		0x2ac
#define INFRA_PROT1_STA1	0x258
#define CONN_PROT_A		((1u << 13) | (1u << 18))	/* first set, raised first */
#define CONN_PROT_B		(1u << 14)			/* first set, raised second */
#define CONN_PROT1		(1u << 10)			/* second set, raised last */
#define INFRA_CG0_SET		0x080		/* clock gates, one gates the clock */
#define INFRA_CG0_CLR		0x084		/* one ungates */
#define INFRA_CG0_STA		0x090
#define INFRA_CG0_BTIF		(1u << 31)
#define INFRA_CG0_PMIC_CONN	(1u << 3)
#define INFRA_CG3_SET		0x0c0
#define INFRA_CG3_CLR		0x0c4
#define INFRA_CG3_STA		0x0c8
#define INFRA_CG3_CCIF4_AP	(1u << 28)	/* the AP side of CONNSYS's mailbox */
#define INFRA_EMI_REMAP		0x380		/* bits 19:0: the DRAM 64 KB page CONNSYS sees at 0xF0000000 */
#define INFRA_PERI_REMAP	0x388		/* bits 19:0: the peripheral page it sees */

#define PERICFG_BASE		0x10003000u
#define PERICFG_CCIF4_CONFIG	0x438		/* bits 1:0: CONNSYS's mailbox status, 1 when it is on */
#define PCCIF4_BASE		0x1024c000u
#define PCCIF4_ACK		0x14		/* all ones clears every pending acknowledgement */

#define TOPRGU_BASE		0x10007000u
#define TOPRGU_SWSYSRST		0x018		/* key 0x88 in bits 31:24 */
#define RGU_KEY			(0x88u << 24)
#define RGU_CONN_CPU		(1u << 12)	/* the CONNSYS CPU held in reset */
#define RGU_CONN_SYS		(1u << 9)

#define CONN_TOP_MISC_OFF	0x180b1000u
#define CONN_IP_VER		(CONN_TOP_MISC_OFF + 0x10)
#define CONN_IP_VER_MT6785	0x10020600u
#define CONN_MCU_BASE		0x18002000u
#define CONN_HW_VER		(CONN_MCU_BASE + 0x0)
#define CONN_FW_VER		(CONN_MCU_BASE + 0x4)
#define CONN_HIF_PDMA_BASE	0x18004000u
#define CONN_SLPPROT_CONTROL	(CONN_HIF_PDMA_BASE + 0x160)	/* bit 4: CONNSYS's own DRAM sleep protection */
#define CONN_TOPPOSE_RESTORE	0x180c1600u			/* bit 23: the restore-done flag */
#define CONN_WPLL_SETTING	0x180b3034u			/* bits 20:19 = 01 */

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
	/* the bus: the protection released in the reverse of the order it is raised */
	R32(INFRACFG_BASE + INFRA_PROT_CLR) = CONN_PROT_B;
	R32(INFRACFG_BASE + INFRA_PROT1_CLR) = CONN_PROT1;
	R32(INFRACFG_BASE + INFRA_PROT_CLR) = CONN_PROT_A;
	g_bt_dbg_step = 4;
	return 0;
}

static void bt_domain_off(void)
{
	unsigned int spm = SPM_BASE;

	R32(INFRACFG_BASE + INFRA_PROT_SET) = CONN_PROT_A;
	bt_wait(INFRACFG_BASE + INFRA_PROT_STA1, CONN_PROT_A, CONN_PROT_A, 10000);
	R32(INFRACFG_BASE + INFRA_PROT_SET) = CONN_PROT_B;
	bt_wait(INFRACFG_BASE + INFRA_PROT_STA1, CONN_PROT_B, CONN_PROT_B, 10000);
	R32(INFRACFG_BASE + INFRA_PROT1_SET) = CONN_PROT1;
	bt_wait(INFRACFG_BASE + INFRA_PROT1_STA1, CONN_PROT1, CONN_PROT1, 10000);
	R32(spm + SPM_POWERON_CONFIG) = (R32(spm + SPM_POWERON_CONFIG) & 0xffffu) | SPM_PROJECT_KEY | 1u;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_ISO;
	R32(spm + SPM_CONN_PWR_CON) |= PWR_CLK_DIS;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_RST_B;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_ON;
	R32(spm + SPM_CONN_PWR_CON) &= ~PWR_ON_2ND;
}

static int s_bt_powered;
static unsigned int s_cg0_was_gated, s_cg3_was_gated;	/* the clock gates power on opened */

/* supplies, pins, clocks, domain, with the CONNSYS CPU kept in reset; 0 when the subsystem
 * answers with its id */
static int kpa_bt_power_on(void)
{
	if (s_bt_powered)
		return 0;
	bt_supplies(1);
	bt_pins(1);
	/* the byte pipe's, the PMIC link's and the mailbox's clocks: the ones that were gated are
	 * gated again at power off */
	s_cg0_was_gated = R32(INFRACFG_BASE + INFRA_CG0_STA) & (INFRA_CG0_BTIF | INFRA_CG0_PMIC_CONN);
	s_cg3_was_gated = R32(INFRACFG_BASE + INFRA_CG3_STA) & INFRA_CG3_CCIF4_AP;
	R32(INFRACFG_BASE + INFRA_CG0_CLR) = INFRA_CG0_BTIF | INFRA_CG0_PMIC_CONN;
	R32(INFRACFG_BASE + INFRA_CG3_CLR) = INFRA_CG3_CCIF4_AP;
	udelay(150);
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) | RGU_CONN_CPU) | RGU_KEY;
	if (bt_domain_on() != 0)
		return -1;
	R32(PERICFG_BASE + PERICFG_CCIF4_CONFIG) = (R32(PERICFG_BASE + PERICFG_CCIF4_CONFIG) & ~3u) | 1u;
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) & ~RGU_CONN_SYS) | RGU_KEY;
	udelay(10);
	g_bt_dbg_step = 5;
	if (bt_wait(CONN_IP_VER, 0xffffffffu, CONN_IP_VER_MT6785, 200000) != 0)
		return -2;
	g_bt_dbg_step = 6;
	s_bt_powered = 1;
	return 0;
}

static void kpa_bt_power_off(void)
{
	R32(TOPRGU_BASE + TOPRGU_SWSYSRST) = (R32(TOPRGU_BASE + TOPRGU_SWSYSRST) | RGU_CONN_CPU) | RGU_KEY;
	R32(PERICFG_BASE + PERICFG_CCIF4_CONFIG) = 0;	/* the mailbox status cleared */
	bt_domain_off();
	udelay(100);					/* any mailbox interrupt in flight lands */
	R32(PCCIF4_BASE + PCCIF4_ACK) = 0xff;
	bt_pins(0);
	bt_supplies(0);
	if (s_cg0_was_gated)				/* the clocks as they were before */
		R32(INFRACFG_BASE + INFRA_CG0_SET) = s_cg0_was_gated;
	if (s_cg3_was_gated)
		R32(INFRACFG_BASE + INFRA_CG3_SET) = s_cg3_was_gated;
	s_bt_powered = 0;
}

/* ---- the hooks pico_bt.inc runs ---- */
#include <platform/boot_mode.h>
#include <arch/arm/mmu.h>
#include "../../emu/gba/fat_ro.h"
#include <platform/pico_bt.h>		/* BT_EMI_PA, BT_EMI_SIZE */
#define PICO_BT_BTIF_BASE	0x1100c000u
#define PICO_BT_IRQ_ID		(138 + 32)	/* SPI 138, active low */
#include <platform/mt_irq.h>
#include <mt_gic.h>
#define PICO_BT_CHIP_ID		0x6785
#define PICO_BT_MODEL		1		/* 2 = the Konkr Pocket Block */
#define PICO_BT_FW_RAM_MCU	"soc1_0_ram_mcu_2a_1_hdr.bin"
#define PICO_BT_FW_RAM_BT	"soc1_0_ram_bt_2a_1_hdr.bin"
#define PICO_BT_FW_RAM_WIFI	"soc1_0_ram_wifi_2a_1_hdr.bin"
#define PICO_BT_FW_PATCH_MCU	"soc1_0_patch_mcu_2a_1_hdr.bin"

static void bt_log(const char *fmt, ...);

static int pico_bt_plat_powered(void) { return s_bt_powered; }
static void pico_bt_plat_off(void) { kpa_bt_power_off(); }

static int pico_bt_plat_start(void)
{
	unsigned int r;

	/* CONNSYS's view of the window and of the peripherals: 64 KB pages on this SoC */
	r = R32(INFRACFG_BASE + INFRA_EMI_REMAP);
	R32(INFRACFG_BASE + INFRA_EMI_REMAP) = (r & ~0xfffffu) | ((BT_EMI_PA >> 16) & 0xfffffu);
	r = R32(INFRACFG_BASE + INFRA_PERI_REMAP);
	R32(INFRACFG_BASE + INFRA_PERI_REMAP) = (r & ~0xfffffu) | ((PERICFG_BASE >> 16) & 0xfffffu);

	if (kpa_bt_power_on() != 0) {
		bt_log("power on failed, step %u", g_bt_dbg_step);
		return 1;
	}
	bt_log("ip %08x hw %04x", R32(CONN_IP_VER), R32(CONN_HW_VER) & 0xffffu);
	R32(CONN_SLPPROT_CONTROL) |= 1u << 4;			/* CONNSYS's DRAM sleep protection */
	R32(CONN_TOPPOSE_RESTORE) |= 1u << 23;			/* the restore-done flag */
	r = R32(CONN_WPLL_SETTING);				/* the radio PLL setting */
	R32(CONN_WPLL_SETTING) = (r & 0xffe7ffffu) | 0x80000u;
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
#define PICO_WIFI_FW_RAM	"WIFI_RAM_CODE_soc1_0_2a_1.bin"
#define PICO_WIFI_RXD_LEN	20		/* this CONNSYS's receive descriptor (the Block's is 16) */
#define PICO_WIFI_HT_5GHZ	0		/* HT on 5 GHz passes no data here yet: legacy rates there */
#include "../common/pico/pico_wifi.inc"

/* power off, before the kernel and the console's own power off: nothing to do when it is off */
void pico_bt_power_off_if_on(void)
{
	if (s_bt_powered)
		pico_bt_off();
}

/* ---- the bootloader channel: this console's diagnostics ---- */
static void bt_report(void)
{
	snprintf(s_line, sizeof s_line, "vcn18 %u/%u vcn33_1 %u/%u sel %u vcn33_2 %u/%u sel %u",
		 pmic_rd(PMIC_VCN18_CON0) & 1, pmic_rd(PMIC_VCN18_MON) & 1,
		 pmic_rd(PMIC_VCN33_1_CON0) & 1, pmic_rd(PMIC_VCN33_1_MON) & 1,
		 (pmic_rd(PMIC_VCN33_1_ANA_CON0) >> 8) & 0xf,
		 pmic_rd(PMIC_VCN33_2_CON0) & 1, pmic_rd(PMIC_VCN33_2_MON) & 1,
		 (pmic_rd(PMIC_VCN33_2_ANA_CON0) >> 8) & 0xf);
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

/* oem btpwr[:0] - power CONNSYS on (CPU in reset) or everything off with :0, and report */
static void cmd_btpwr(const char *arg, void *data, unsigned sz)
{
	int rc = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == '0') {
		/* Wi-Fi first, the regular way: the AP joined is told we left (a radio cut while
		 * joined left it holding us, and the next join never got its key handshake) */
		{ extern void netsvc_wifi(int on); netsvc_wifi(0); }
		pico_bt_off();
	}
	else
		rc = kpa_bt_power_on();
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

void kpa_bt_register_cmds(void)
{
	pico_bt_register_cmds();
	fastboot_register("oem btpwr", cmd_btpwr, 1, 0);
	fastboot_register("oem btrd:", cmd_btrd, 1, 0);
	pico_wifi_register_cmds();
}
