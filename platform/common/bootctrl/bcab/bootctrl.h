/*
 * bootctrl.h - the bootctrl entry points (AOSP bootloader_control version, bootctrl_api.c).
 */
#ifndef _BOOTCTRL_H_
#define _BOOTCTRL_H_

#include <stdint.h>

#define BOOTCTRL_SUFFIX_A      "_a"
#define BOOTCTRL_SUFFIX_B      "_b"
#define BOOTCTRL_SUFFIX_MAXLEN 2

const char *get_suffix(void);
int set_active_slot(const char *suffix);
uint8_t get_retry_count(const char *suffix);
int get_bootup_status(const char *suffix);
int get_bootable_status(const char *suffix);
int increase_tries_remaining(void);

#endif /* _BOOTCTRL_H_ */
