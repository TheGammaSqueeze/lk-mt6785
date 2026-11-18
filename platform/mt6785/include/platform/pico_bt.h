/*
 * pico_bt.h - what other parts of the Konkr Pocket Advance's LK must know of Bluetooth.
 *
 * CONNSYS (the MT6785's connectivity subsystem, kpa_bt.c) reads its firmware from a window of
 * DRAM it sees through a remap register (64 KB pages on this SoC). The rewind arena
 * (platform/common/pico/ayaneo_rewind.c) leaves this window, and the Wi-Fi DMA memory after it,
 * out of the memory it takes, whatever the console's DRAM size, and RetroAchievements' memory after
 * that.
 */
#ifndef PICO_BT_H
#define PICO_BT_H

#define BT_EMI_PA	0xBE000000u	/* the same place as on the Konkr Pocket Block */
#define BT_EMI_SIZE	0x00400000u
/* the Wi-Fi block's DMA memory (its rings and buffers, pico_wifi.inc): right after the firmware
 * window, and left out of the rewind arena with it */
#define WIFI_DMA_PA	(BT_EMI_PA + BT_EMI_SIZE)
#define WIFI_DMA_SIZE	0x00200000u
/* RetroAchievements (cores/ra-pico, emu/gba/pico_ra.c): the blob, its heap and the LK's buffers for
 * it, after the Wi-Fi memory and left out of the rewind arena too */
#define RA_MEM_PA	(WIFI_DMA_PA + WIFI_DMA_SIZE)
#define RA_MEM_SIZE	0x00400000u
#define PICO_NET_MEM_END	(RA_MEM_PA + RA_MEM_SIZE)	/* the end of all of it */

#endif
