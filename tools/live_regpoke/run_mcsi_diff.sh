#!/bin/bash
#
# Live MCSI snoop-admission hotplug-diff. Reads the MCSI (Mediatek Cache Snoop
# Interconnect) register file THROUGH ATF (secure SMC MCSI_A_READ=0x8200028A),
# with a target core hotplug ONLINE vs OFFLINE. The register bits that toggle are
# the snoop-admission control the LK worker bringup must reproduce. mcusys/mcucfg
# are secure-protected, so a raw ioremap readl sees 0 - this MUST go via ATF.
#
# Usage: ./run_mcsi_diff.sh <serial> <cpuN> [sweepEnd]
set -e
SERIAL="${1:?serial}"
CPU="${2:-1}"
SWEEP="${3:-0x200}"
A="adb -s $SERIAL"
KO=/data/local/tmp/smcpoke.ko

$A push smcpoke.ko $KO >/dev/null
run() { $A shell "dmesg -c >/dev/null 2>&1; insmod $KO fid=0x8200028A a0=0x0 sweep=$SWEEP tag=$1; dmesg | grep SMCPOKE" ; }

echo "== MCSI dump, cpu$CPU ONLINE =="
$A shell "echo 1 > /sys/devices/system/cpu/cpu$CPU/online" 2>/dev/null || true
run online | tee /tmp/mcsi_online.txt

echo "== MCSI dump, cpu$CPU OFFLINE =="
$A shell "echo 0 > /sys/devices/system/cpu/cpu$CPU/online"
run offline | tee /tmp/mcsi_offline.txt

echo "== bring cpu$CPU back online =="
$A shell "echo 1 > /sys/devices/system/cpu/cpu$CPU/online"

echo "== DIFF (online vs offline; lines that differ = snoop-admission bits) =="
diff <(sed 's/tag=[a-z]*//' /tmp/mcsi_online.txt) <(sed 's/tag=[a-z]*//' /tmp/mcsi_offline.txt) || true
