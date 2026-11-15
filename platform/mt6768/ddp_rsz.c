/*
 * The display resizer (DISP_RSZ0, 0x14015000) as a module of the GammaOS Pico display path on the
 * Konkr Pocket Block: OVL0_2L -> RSZ -> OVL0 -> RDMA0 -> DSI0, the order the KPA uses. The
 * module only owns the resizer's clock; GammaOS Pico programs the scale itself per game
 * (pico_disp.inc snes_rsz_program) and those registers then persist across the per frame layer
 * configs, so this driver has no config hook. At init the resizer is left as a disabled 1:1
 * pass through of the full panel, the state pico_disp.inc's ayaneo_snes_rsz_restore returns it
 * to, so the boot logo and the menu go through it unscaled.
 */

#if defined(AYANEO_GBA_SD)
#define LOG_TAG "RSZ"

#include <platform/ddp_hal.h>
#include <platform/ddp_reg.h>
#include <platform/ddp_info.h>
#include <platform/ddp_log.h>
#include <platform/pico_hw.h>
#include <platform/disp_drv.h>
#include <target/board.h>

#define BLOCK_RSZ(off)	(*(volatile unsigned int *)(PICO_RSZ0_BASE + (off)))

static void rsz_pass_through(void)
{
	unsigned int size = ((unsigned int)CFG_DISPLAY_HEIGHT << 16) | (unsigned int)CFG_DISPLAY_WIDTH;

	BLOCK_RSZ(0x004) = 0;		/* CONTROL_1: no H or V scale */
	BLOCK_RSZ(0x010) = size;	/* INPUT_IMAGE  = h << 16 | w */
	BLOCK_RSZ(0x014) = size;	/* OUTPUT_IMAGE */
	BLOCK_RSZ(0x000) = PICO_RSZ_IDLE_EN;			/* ENABLE, at 1:1 */
	BLOCK_RSZ(0x0f0) = 0x2;						/* commit to the active copy */
}

static int block_rsz_power_on(DISP_MODULE_ENUM module, void *handle)
{
	ddp_enable_module_clock(module);
	return 0;
}

static int block_rsz_power_off(DISP_MODULE_ENUM module, void *handle)
{
	ddp_disable_module_clock(module);
	return 0;
}

static int block_rsz_init(DISP_MODULE_ENUM module, void *handle)
{
	ddp_enable_module_clock(module);
	rsz_pass_through();
	return 0;
}

static int block_rsz_deinit(DISP_MODULE_ENUM module, void *handle)
{
	ddp_disable_module_clock(module);
	return 0;
}

DDP_MODULE_DRIVER ddp_driver_rsz = {
	.module		= DISP_MODULE_RSZ,
	.init		= block_rsz_init,
	.deinit		= block_rsz_deinit,
	.power_on	= block_rsz_power_on,
	.power_off	= block_rsz_power_off,
};
#endif /* AYANEO_GBA_SD */
