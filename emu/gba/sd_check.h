/*
 * sd_check.h - is every cluster on the card used by at most one file (oem sd-check)?
 */
#ifndef SD_CHECK_H
#define SD_CHECK_H

#include "fat_ro.h"

/* Walk the card; out gets a summary line, then a line for every file holding a cluster another
 * file holds too. Returns the clusters used twice, or -1 without the scratch memory. */
int sd_check_run(fat_vol *v, void (*out)(const char *line));

#endif
