#!/bin/bash
#
# Cross-DSU coherency hotplug-diff (task a, done right). MT6785 has two physical DSU
# clusters by MPIDR aff1: DSU0={cpu0-3}, DSU1={cpu4-7}. The LK worker is cpu4 (mpidr
# 0x100) in DSU1. To catch the register that admits DSU1 into the coherency domain, we
# must power the WHOLE DSU1 off (offline cpu4,5,6,7) vs on - offlining only cpu4 leaves
# DSU1 powered (just that core gated), so only its power bit would move.
#
# Dumps mcucfg (0x0c530000, 64KB) AND mcucci (0x0c510000, 4KB) nonzero-only via regpoke,
# with DSU1 fully OFF vs ON, and diffs. Any reg that differs beyond the per-cpu power
# bits is the cross-DSU snoop/coherency-admission control the LK bringup must reproduce.
#
# Usage: ./run_mcucfg_diff.sh <serial>
set -e
SERIAL="${1:?serial}"
A="adb -s $SERIAL"
KO=/data/local/tmp/regpoke.ko
$A push regpoke.ko $KO >/dev/null

dump() { # tag
  $A shell "dmesg -c >/dev/null 2>&1;
    insmod $KO base=0x0c530000 n=16384 nz=1 tag=mcucfg_$1;
    insmod $KO base=0x0c510000 n=1024  nz=1 tag=mcucci_$1;
    dmesg | grep REGPOKE"
}

echo "== baseline: all cores online (DSU1 ON) =="
for c in 4 5 6 7; do $A shell "echo 1 > /sys/devices/system/cpu/cpu$c/online" 2>/dev/null || true; done
dump on | tee /tmp/mcucfg_on.txt

echo "== power DSU1 fully OFF (offline cpu4,5,6,7) =="
for c in 7 6 5 4; do $A shell "echo 0 > /sys/devices/system/cpu/cpu$c/online"; done
$A shell 'cat /sys/devices/system/cpu/online'
dump off | tee /tmp/mcucfg_off.txt

echo "== restore: bring DSU1 back online =="
for c in 4 5 6 7; do $A shell "echo 1 > /sys/devices/system/cpu/cpu$c/online"; done

echo "== DIFF (ON vs OFF; lines present/changed = DSU1 coherency+power admission regs) =="
diff <(sed 's/tag=[a-z0-9_]*//' /tmp/mcucfg_on.txt | sort) \
     <(sed 's/tag=[a-z0-9_]*//' /tmp/mcucfg_off.txt | sort) || true
echo "(known power-gating bits will move too; the SNOOP/coherency reg is the one NOT in the SPM/CPC power block near 0x0c53a700)"
