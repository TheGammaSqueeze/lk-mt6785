/*
 * GammaOS Pico multi-core (SMP) support for the LK.
 *
 * DEFAULT IS SINGLE CORE. The LK boots and runs on the boot core only; nothing
 * here is called at boot. This module is the opt-in path for powering the other
 * CPU cores so that heavy work can later be shifted onto them.
 *
 * It only works on a device whose secure firmware has been patched to arm the
 * CPU power controller at cold boot (see tools/konkr/firmware/). On stock
 * firmware pico_smp_core_up() returns an error (the power-on would hang), so it
 * is always safe to call: it is a no-op that reports "not available".
 *
 * The on-device bring-up, data exchange and cache-coherency were validated via
 * the `oem smp` fastboot commands (see pico_smp.c). A core comes up in AArch64
 * EL1; once it enables its caches with Inner-Shareable memory it is fully
 * cache-coherent with the AArch32 boot core through the DSU, so shared-memory
 * offload needs no per-use cache maintenance.
 *
 * Workload offload itself is not wired yet: by design we stay single-core until
 * a specific hot path is chosen to move onto a worker.
 */
#ifndef PICO_SMP_H
#define PICO_SMP_H

/* Linear core ids on this SoC: 0 = boot core; 1..5 little (A55); 6,7 big (A76).
 * MPIDR of core N is 0x8100_0N00. */
#define PICO_SMP_CORE_BOOT   0u
#define PICO_SMP_BIG0        6u
#define PICO_SMP_BIG1        7u

/* Register the `oem smp` debug/bring-up commands. Called from the fastboot
 * command table; registering the commands does NOT power any core. */
void pico_smp_register_cmds(void);

/* True if the secure firmware arms the CPU power controller at cold boot (i.e.
 * the tee CPC patch is applied): CPC_CTRL_ENABLE set in MCUCFG_CPC_FLOW_CTRL.
 * When false, pico_smp_core_up() will not attempt a power-on (it would hang). */
int pico_smp_available(void);

/* Power core `n` (1..7) and start it at the given AArch64 entry, handing it the
 * shared-block and page-table physical bases through the SRAM parameter page.
 * Returns 0 on PSCI success. Safe to call on stock firmware: returns non-zero
 * without touching the core when pico_smp_available() is false. */
int pico_smp_core_up(unsigned n, unsigned entry_pa, unsigned shared_pa, unsigned pt_pa);

/* Ask all running workers to stop (they self power-off via PSCI CPU_OFF) and
 * wait until the power-status siblings clear. */
void pico_smp_stop_all(void);

/* The GBA experiment (opt in, off by default): g_pico_smp_gba_req asks the next GBA game to run
 * its CPU loop on core g_pico_smp_gba_core. pico_smp_guest_start runs 32 bit code at `entry`
 * on core n, unprivileged, on the given stack. */
extern volatile int g_pico_smp_gba_req;
extern volatile unsigned g_pico_smp_gba_core;
extern volatile unsigned g_pico_smp_gba_mhz;   /* big-core clock while a GBA guest runs (default 1600) */
#define g_pico_smp_comm ((volatile unsigned *)0x4CDC0000u)   /* fixed DRAM, see pico_smp.c */
int pico_smp_guest_start(unsigned n, unsigned entry, unsigned stack_top);
/* pico_smp_guest_held: the host holds the guest (it faulted, or its frame watchdog cut it off).
 * A held guest starts again with pico_smp_guest_restart; the core stays powered. 0 = done. */
int pico_smp_guest_held(void);
int pico_smp_guest_restart(unsigned entry, unsigned stack_top);
int pico_smp_core_on(unsigned n);
/* The big core's clock for the menus: the levels offered (0 when the console has no table yet), and
 * the setter (one of those levels; its stock voltage goes with it). */
int pico_smp_big_levels(void);
unsigned pico_smp_big_level_mhz(int i);
void pico_smp_big_clock(unsigned mhz);

/* Called by the reset and power off paths: stops the workers if any core was started. */
void pico_smp_before_reset(void);

/* The reset-proof mark page (the tail of the RAM console SRAM), 16 words. Words 12..15 are the GBA
 * frontend's and guest's breadcrumbs: 12 frontend stage, 13 frames asked, 14 last wait (us),
 * 15 frames the guest finished. Read with `oem smp:bc` after a restart. */
#if defined(KONKR_BLOCK)
#define PICO_SMP_SRAM 0x0010EF00u
#else
#define PICO_SMP_SRAM 0x0011D7C0u
#endif
/* The free megabyte of DRAM after the LK's own nine (0x4CD00000, the same on both Konkr consoles):
 *   +0x00000  76.8 KB  the GBA intro's recoloured frame (gba_driver.c)
 *   ...                the guest's stack, growing down from +0xC0000
 *   +0xC0000           the shared block, the guest's tables, the workers' buffers (pico_smp.c) */
#define PICO_FREE_MB_BASE 0x4CD00000u
/* The guest's own stack: the free DRAM below the shared buffers (0x4CD00000..0x4CDC0000, 768 KB),
 * growing down from them. Never the parked CPU thread's stack: that holds the thread's saved
 * context, and the next game that wakes the thread would crash on it. */
#define PICO_SMP_GUEST_STACK_TOP 0x4CDC0000u
#define PICO_SMP_MARK(i, v) (((volatile unsigned *)PICO_SMP_SRAM)[i] = (unsigned)(v))

#endif /* PICO_SMP_H */
