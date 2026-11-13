/*
 * pico_version.c - GammaOS Pico's version and the commit it was built from (the VERSION file and
 * git, through the build's version.h), for the menus. Alone in its file: version.h changes every
 * build, and only this is built again for it.
 */
#include <version.h>
#include "pico_version.h"

const char *pico_version(void) { return PICO_VERSION; }
const char *pico_commit(void) { return PICO_COMMIT; }
const char *pico_build_day(void) { return PICO_BUILD_DAY; }
