/*
 * pico_version.h - GammaOS Pico's version: "1.0.0", the commit ("660b788", with "+" after it for a
 * build of changes not committed), and the day it was built ("2026-11-13").
 */
#ifndef PICO_VERSION_H
#define PICO_VERSION_H

const char *pico_version(void);
const char *pico_commit(void);
const char *pico_build_day(void);

#endif
