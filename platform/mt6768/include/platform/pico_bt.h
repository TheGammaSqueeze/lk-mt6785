/*
 * pico_bt.h - what other parts of the Konkr Pocket Block's LK must know of Bluetooth.
 *
 * CONNSYS (the MT6768's connectivity subsystem, block_bt.c) reads its firmware from a window of
 * DRAM it sees through a remap register, 16 MB page granular. The rewind arena
 * (platform/common/pico/ayaneo_rewind.c) leaves this window, and the Wi-Fi DMA memory after it,
 * out of the memory it takes, whatever the console's DRAM size, and RetroAchievements' memory after
 * that.
 */
#ifndef PICO_BT_H
#define PICO_BT_H

#define BT_EMI_PA	0xBE000000u	/* as Android places it; 2 MB aligned, inside the top 2 GB */
#define BT_EMI_SIZE	0x00400000u
/* the Wi-Fi block's DMA memory (its rings and buffers, pico_wifi.inc): right after the firmware
 * window, and left out of the rewind arena with it */
#define WIFI_DMA_PA	(BT_EMI_PA + BT_EMI_SIZE)
#define WIFI_DMA_SIZE	0x00200000u
/* RetroAchievements (cores/ra-pico, emu/gba/pico_ra.c): the blob, its heap and the LK's buffers for
 * it, after the Wi-Fi memory and left out of the rewind arena too */
#define RA_MEM_PA	(WIFI_DMA_PA + WIFI_DMA_SIZE)
/* as much of the next 16 MB as the console's DRAM block holds there, at least 4 MB (pico_ra.c picks
 * it at its first use): a game with a big set (Mario & Luigi: Superstar Saga, three sets and 226
 * achievements) needs about 2 MB of the blob's heap, more than 4 MB less our 2 MB of buffers left */
#define RA_MEM_MIN	0x00400000u
#define RA_MEM_MAX	0x01000000u
#define PICO_NET_MEM_END	(RA_MEM_PA + RA_MEM_MAX)	/* the end of all of it (the rewind arena leaves it out) */

#endif
