#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Manual, temporary CPU test. No install, service, autoload or VPN changes.
set -eu

BASE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
KO="$BASE/mt7981_oc_hold.ko"
MODULE=mt7981_oc_hold
STATE=/sys/module/mt7981_oc_hold/parameters/state
OWNER=/tmp/mt7981-oc-cli.owner
# Same lock as the separate panel tool, to refuse concurrent transitions.
LOCK=/tmp/mt7981-oc-control.lock
SHA=$(sha256sum "$KO" | awk '{print $1}')
locked=0
new_load=0

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
usage() {
 printf '%s\n' \
  'Usage: sh ./oc.sh check | status | 1300 | MHz [seconds] [--risk]' \
  'MHz: integer 1301..2000; above 1700 requires --risk. 1300 = stock return.' \
  'Test timer: disabled by default; optional 30..1800 seconds; 0 = until return/reboot/guard.' \
  'EXPERIMENTAL. A complete CPU freeze cannot be repaired by this software.' \
  'After a freeze, a power cycle may be required. Do not add this to startup.'
}
owned() {
 test -f "$OWNER" &&
 test "$(cat "$OWNER")" = "$SHA $(cat /proc/sys/kernel/random/boot_id)"
}
platform_check() {
 test "$(id -u)" = 0 || die 'Run as root over SSH.'
 for cmd in insmod rmmod jsonfilter sha256sum apk awk grep; do
  command -v "$cmd" >/dev/null || die "Missing required command: $cmd. Nothing installed."
 done
 test -f /tmp/sysinfo/board_name || die 'OpenWrt board identity is missing.'
 test "$(uname -m)" = aarch64 || die 'Wrong architecture.'
 test -r /etc/openwrt_release || die 'OpenWrt release identity is missing.'
 # This is the existing trusted OS release file, never package-provided code.
 # shellcheck disable=SC1091
 . /etc/openwrt_release
 test "${DISTRIB_TARGET:-}" = mediatek/filogic || die 'Different target.'
 test "$(cat /sys/devices/system/cpu/online)" = 0-1 || die 'Exactly two online CPUs are required.'
 test -f "$KO" || die 'Put oc.sh and mt7981_oc_hold.ko in the same extracted directory.'
}
status() {
 if [ -r "$STATE" ];then
  if owned;then echo 'Module owner: this standalone CLI';else echo 'Module owner: external (this CLI will not unload or replace it)';fi
  cat "$STATE"
 else
  echo 'OC module is not loaded. This does not prove raw PLL state.'
  if [ -r /sys/kernel/debug/clk/armpll/clk_rate ];then
   printf 'CCF cached rate: ';cat /sys/kernel/debug/clk/armpll/clk_rate
  fi
 fi
 for z in /sys/class/thermal/thermal_zone*;do
  [ -r "$z/type" ] || continue
  if [ "$(cat "$z/type")" = cpu-thermal ];then
   printf 'CPU temperature (millidegrees C): ';cat "$z/temp"
  fi
 done
 awk '/^MemAvailable:/' /proc/meminfo
}
on_exit() {
 rc=$?
 trap - EXIT HUP INT TERM
 if [ "$rc" -ne 0 ] && [ "$new_load" = 1 ] && [ -d /sys/module/mt7981_oc_hold ];then
  echo 'CLI verification failed. Requesting module unload/stock return.' >&2
  if rmmod "$MODULE";then rm -f "$OWNER";else
   echo 'Unload failed. Do not force unload. A power cycle may be required.' >&2
  fi
 fi
 if [ ! -d /sys/module/mt7981_oc_hold ] && owned;then rm -f "$OWNER";fi
 if [ "$locked" = 1 ];then rmdir "$LOCK" 2>/dev/null || true;fi
 exit "$rc"
}
stock_probe() {
 # target_mhz=1300 is the existing read-only branch: no PLL writes.
 new_load=1
 insmod "$KO" run_test=1 target_mhz=1300 hold_seconds=0 || die 'Stock register probe failed. No OC was requested.'
 test "$(jsonfilter -i "$STATE" -e '@.valid')" = true || die 'Invalid stock register snapshot.'
 test "$(jsonfilter -i "$STATE" -e '@.effective_mhz')" = 1300 || die 'Not at the expected stock frequency.'
 test "$(jsonfilter -i "$STATE" -e '@.bus')" = 000e0201 || die 'Unexpected CPU mux.'
 test "$(jsonfilter -i "$STATE" -e '@.aclken')" = 00000012 || die 'Unexpected divider.'
 test "$(jsonfilter -i "$STATE" -e '@.con0')" = 00670111 || die 'Unexpected ARMPLL_CON0.'
 test "$(jsonfilter -i "$STATE" -e '@.con1')" = 82000000 || die 'Unexpected ARMPLL_CON1.'
 cat "$STATE"
 rmmod "$MODULE" || die 'Stock probe unload failed.'
 new_load=0
}
set_frequency() {
 mhz=$1;seconds=$2;risk=$3
 platform_check
 # Validate BEFORE touching a currently running OC module.
 case "$seconds" in ''|*[!0-9]*) die 'Timer must be an integer.';;esac
 test "${#seconds}" -le 4 || die 'Timer is too large.'
 seconds=$(awk -v s="$seconds" 'BEGIN {printf "%d",s+0}')
 if [ "$seconds" != 0 ];then
  test "$seconds" -ge 30 && test "$seconds" -le 1800 || die 'Timer must be 0 or 30..1800 seconds.'
 fi
 if [ "$mhz" -gt 1700 ];then
  test "$risk" = 1 || die 'Above 1700 MHz is not recommended. Explicit --risk is required.'
  echo 'WARNING: above 1700 MHz is unverified and not recommended; freeze, damage or data loss are possible.' >&2
 fi
 if [ -d /sys/module/mt7981_oc_hold ] && ! owned;then
  die 'An external OC module is loaded. Use its own 1300/return control first. This CLI will not unload it.'
 fi
 mkdir "$LOCK" 2>/dev/null || die 'Another CPU transition is running. Do not remove a live lock.'
 locked=1
 trap on_exit EXIT
 trap 'exit 129' HUP
 trap 'exit 130' INT
 trap 'exit 143' TERM
 if [ -d /sys/module/mt7981_oc_hold ];then
  rmmod "$MODULE" || die 'Cannot unload the CLI-owned OC module. Do not force it.'
  rm -f "$OWNER"
 fi
 stock_probe
 if [ "$mhz" = 1300 ];then echo 'Verified stock 1300 MHz; module unloaded.';return;fi
 test "$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)" -ge 32768 || die 'Less than 32 MiB available. Test refused.'
 echo "EXPERIMENTAL: requesting $mhz MHz; timer=$seconds s; temperature guard=74 C."
 echo 'Firmware/bootloader/DT/voltage, VPN and autostart will not be changed.'
 new_load=1
 insmod "$KO" run_test=1 target_mhz="$mhz" hold_seconds="$seconds" allow_unsafe="$risk" || die 'OC module rejected the transition. Check dmesg.'
 (umask 077;printf '%s %s\n' "$SHA" "$(cat /proc/sys/kernel/random/boot_id)" >"$OWNER")
 sleep 1
 test "$(jsonfilter -i "$STATE" -e '@.active')" = true || die 'OC is not active (the guard may have returned it).'
 test "$(jsonfilter -i "$STATE" -e '@.valid')" = true || die 'Raw register verification failed.'
 test "$(jsonfilter -i "$STATE" -e '@.effective_mhz')" = "$mhz" || die 'Actual frequency does not match.'
 status
 echo 'Verified. To return: sh ./oc.sh 1300'
}

case "${1:-}" in
 status) test "$#" = 1 || { usage;exit 2; };status;;
 check) test "$#" = 1 || { usage;exit 2; };platform_check;echo 'Compatibility/checksum PASS. Check did not load modules or write registers.';status;;
 1300) test "$#" = 1 || { usage;exit 2; };set_frequency 1300 0 0;;
 *)
  test "$#" -ge 1 && test "$#" -le 3 || { usage;exit 2; }
  mhz=$1
  case "$mhz" in ''|*[!0-9]*) usage;exit 2;;esac
  test "${#mhz}" = 4 || die 'Frequency must be an integer 1301..2000 MHz.'
  test "$mhz" -gt 1300 && test "$mhz" -le 2000 || die 'Frequency must be 1301..2000 MHz; use 1300 for stock return.'
  seconds=0;risk=0
  if [ "$#" -ge 2 ];then
   if [ "$2" = --risk ];then
    test "$#" = 2 || { usage;exit 2; }
    risk=1
   else
    seconds=$2
    if [ "$#" = 3 ];then
     test "$3" = --risk || { usage;exit 2; }
     risk=1
    fi
   fi
  fi
  set_frequency "$mhz" "$seconds" "$risk"
  ;;
esac
