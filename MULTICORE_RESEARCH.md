# MT6785 multi-core-at-LK revival research

Live research log. Goal: get a 2nd (or more) CPU core doing useful work during the LK-stage
SNES menu (2 cores at a lower clock -> 60fps at lower power, or just more compute). This
revisits a path previously concluded a "dead end" with fresh eyes and a specific new lever.

Repo: /work/svoboda_lk (branch lk-snes-menu). Kernel src: /work/mt6785_kernel_source.
ATF disasm: /tmp/atf.dis, /tmp/bl31_dis.txt (tee.img BL31, base 0x4ce00000, dis = file-0x400).
MTK ATF source (same-family mt8186/mt8192): /tmp/mtk-atf. SoC = MT6785 / Helio G95:
cluster0 = 6x Cortex-A55 (MPIDR 0x000..0x500), cluster1 = 2x Cortex-A75 (0x600/0x700),
single DSU (DynamIQ). Boot core = cpu0 (0x000, A55). LK = NS BL33 under resident EL3 ATF.

## THE WALL (exhaustively proven on HW in the prior effort)
- 2nd core (cpu1, A55, reg 0x100, same cluster0/DSU as cpu0) CAN be powered on + brought up
  cached (adopts cpu0's LPAE MMU snapshot, D/I-cache + VFP on) via: ATF cold-boot patch that
  arms the CPC power controller (tee.img 4-byte @0x9688: bl NOTICE -> bl 0x94d0 spmc/CPC arm)
  + re-sign cert2, then plain PSCI CPU_ON(0x100) from LK. Confirmed alive + cached on HW.
- worker -> cpu0 WRITES work (worker cleans to DRAM/PoC, cpu0 invalidates + reads: correct).
- cpu0 -> worker READS FAIL the instant the worker's MMU is ON. Proven for EVERY memory
  attribute: WB Inner-Shareable, Device-nGnRE, strongly-ordered+Outer-Shareable, D-cache-off.
  Worker reads a FIXED stale/uninit value; faults on garbage menu_ptr. At MMU-OFF the worker
  reads cpu0's (DRAM-resident, cleaned) data fine (that's how it adopts the MMU snapshot).
- Root cause (prior conclusion): the CPC-hardware power-on bypass powers the core + wires its
  WRITE channel but never admits the core as a DSU snoop-READ participant. That admission is
  done by the SSPM-driven SPMC power sequence, which is DORMANT at LK stage (it's why a plain
  full PSCI CPU_ON HANGS at LK without the CPC-arm bypass - SSPM never acks the power-on).
- Tried + failed to fix at LK: ATF w21-ungate patch to force the full per-core
  pwr_domain_on_finish body (no change - the CCI/DSU admit routines it gates are CLUSTER-level
  fixed addrs, already run for cluster0 at cpu0 boot; there is NO per-core snoop-enable reg
  that cpu0 programs for cpu1). fake-kernel-jump (spmc_init is one-shot, already ran). Every
  memory attribute. So: "the worker never becomes a DSU snoop-read participant; that requires
  the SSPM-driven power sequence that only runs at kernel stage."

## THE NEW LEVER (why this might not actually be dead)
The kernel gets coherent secondaries "for free" because by kernel time the SSPM coprocessor is
LIVE and actively SERVICING CPU-power (the full SPMC sequence that admits each core to DSU
snoop). The unexplored question: **what exactly makes SSPM service coherent CPU-power, and can
LK trigger/replicate it?** Prior effort deemed this "mechanism unclear, likely not NS-reachable"
but did NOT deeply RE the kernel's SSPM/MCDI/SPM init to find the actual trigger. That is the
focus now. Sub-questions:
1. Is SSPM already RUNNING at LK (preloader-loaded) but not servicing CPU-power until commanded,
   or does the kernel boot/enable it? What is the exact enable trigger (mailbox IPC? register)?
2. What does the kernel do on cpu_up beyond `psci_cpu_on -> smc`? Any SSPM/SPM command that
   arms per-core snoop admission?
3. Can that trigger be issued from LK (NS PL1) directly, via an existing ATF SiP, or by porting
   the SSPM driver / setting MTK_TINYSYS_SSPM_SUPPORT=yes in LK?
4. Reconsider the "MMU-on read fails even for Device" conclusion - is there a hole? (The worker
   reading back its OWN MMU-on write to shared DRAM was never definitively done.)
5. Feasibility of the A75 big cores (0x600/0x700, separate powered-off cluster1) vs the A55.

## STATUS
- 2026-08-28: research restarted (user request). Prior GPU/OVL SNES work paused + reverted to a
  correct state (staged). Launching a deep parallel RE (kernel SSPM/MCDI/SPM + ATF + online +
  frankenstein brainstorm). Findings appended below.

## FINDINGS
(appended per research cycle)
