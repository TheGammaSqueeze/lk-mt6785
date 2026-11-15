#ifndef PICO_HW_H
#define PICO_HW_H

/* GammaOS Pico: the display blocks the shared display code (platform/common/pico/pico_disp.inc)
 * programs directly, on the MT6768 (Konkr Pocket Block). OVL0, OVL0_2L from mt_reg_base.h; the
 * resizer from the console's own device tree (/soc/disp_rsz0@14015000; the DPI_BASE that
 * mt_reg_base.h puts at that address is a generic leftover). */
#define PICO_OVL0_BASE		0x1400B000u
#define PICO_OVL0_2L_BASE	0x1400C000u
#define PICO_RSZ0_BASE		0x14015000u
/* MT6768's resizer passes nothing while disabled: between games it stays enabled at 1:1 */
#define PICO_RSZ_IDLE_EN	1u
/* MT6768's display resizer scales a line of at most 736 input pixels in one pass; the pixels
 * past that came out as a band of garbage at the right edge (SNES at 768 wide). The game is
 * prescaled no wider than this and the resizer finishes the rest. */
#define PICO_RSZ_MAX_IN_W	736u

#endif
