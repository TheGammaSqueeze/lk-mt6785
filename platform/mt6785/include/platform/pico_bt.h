/*
 * pico_bt.h - what other parts of the Konkr Pocket Advance's LK must know of Bluetooth.
 *
 * CONNSYS (the MT6785's connectivity subsystem, kpa_bt.c) reads its firmware from a window of
 * DRAM it sees through a remap register (64 KB pages on this SoC). The rewind arena
 * (platform/common/pico/ayaneo_rewind.c) leaves this window out of the memory it takes, whatever
 * the console's DRAM size.
 */
#ifndef PICO_BT_H
#define PICO_BT_H

#define BT_EMI_PA	0xBE000000u	/* the same place as on the Konkr Pocket Block */
#define BT_EMI_SIZE	0x00400000u

#endif
