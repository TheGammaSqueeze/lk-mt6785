/*
 * pico_bt.h - what other parts of the Konkr Pocket Block's LK must know of Bluetooth.
 *
 * CONNSYS (the MT6768's connectivity subsystem, block_bt.c) reads its firmware from a window of
 * DRAM it sees through a remap register, 16 MB page granular. The rewind arena
 * (platform/common/pico/ayaneo_rewind.c) leaves this window out of the memory it takes, whatever
 * the console's DRAM size.
 */
#ifndef PICO_BT_H
#define PICO_BT_H

#define BT_EMI_PA	0xBE000000u	/* as Android places it; 2 MB aligned, inside the top 2 GB */
#define BT_EMI_SIZE	0x00400000u

#endif
