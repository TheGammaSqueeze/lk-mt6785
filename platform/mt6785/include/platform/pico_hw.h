#ifndef PICO_HW_H
#define PICO_HW_H

/* GammaOS Pico: the display blocks the shared display code (platform/common/pico/pico_disp.inc)
 * programs directly, on the MT6785. From the vendor kernel's device tree ("mediatek,disp_rsz0"):
 * the display resizer is DISP_RSZ0 at 0x1401A000, not MDP_RSZ0 (0x14003000) and not the
 * 0x14002000 project.h names (that is MDP_RDMA1). */
#define PICO_OVL0_BASE		0x14008000u	/* the layer the menus and overlays are on */
#define PICO_OVL0_2L_BASE	0x14009000u	/* the game layer, ahead of the resizer */
#define PICO_RSZ0_BASE		0x1401A000u	/* the display resizer between them */

#endif
