#!/bin/bash
set -u
C=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
E=$C/env
uname -a > $E/uname.txt
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE,MAXMHZ,MINMHZ > $E/lscpu-e.txt
lscpu > $E/lscpu.txt
lscpu -C > $E/lscpu-caches.txt
{
for i in 0 1 2 3; do
  d=/sys/devices/system/cpu/cpu7/cache/index$i
  echo "index$i level=$(cat $d/level) type=$(cat $d/type) size=$(cat $d/size) ways=$(cat $d/ways_of_associativity) line=$(cat $d/coherency_line_size) shared_cpu_list=$(cat $d/shared_cpu_list)"
done
} > $E/caches-cpu7-sysfs.txt
{
for c in 7 15; do
  d=/sys/devices/system/cpu/cpu$c
  echo "cpu$c online=$(cat $d/online) core_id=$(cat $d/topology/core_id) thread_siblings_list=$(cat $d/topology/thread_siblings_list) node=$(ls -d $d/node* | sed 's/.*node//')"
done
echo "smt_active=$(cat /sys/devices/system/cpu/smt/active) smt_control=$(cat /sys/devices/system/cpu/smt/control)"
} > $E/affinity-smt.txt
{
for c in 7 15; do
  d=/sys/devices/system/cpu/cpu$c/cpufreq
  echo "cpu$c driver=$(cat $d/scaling_driver) governor=$(cat $d/scaling_governor) min_khz=$(cat $d/scaling_min_freq) max_khz=$(cat $d/scaling_max_freq) cpuinfo_min=$(cat $d/cpuinfo_min_freq) cpuinfo_max=$(cat $d/cpuinfo_max_freq) epp=$(cat $d/energy_performance_preference 2>/dev/null)"
done
echo "boost=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)"
echo "amd_pstate_status=$(cat /sys/devices/system/cpu/amd_pstate/status 2>/dev/null)"
echo "published_campaign_v38_manifest: $(grep -h governor $C/../../raw/v38/MANIFEST | head -1)"
} > $E/frequency-policy.txt
{
echo "thp_enabled=$(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "thp_defrag=$(cat /sys/kernel/mm/transparent_hugepage/defrag)"
echo "nr_hugepages=$(cat /proc/sys/vm/nr_hugepages)"
grep -E 'HugePages|Hugepagesize|Hugetlb|AnonHuge' /proc/meminfo
} > $E/pages.txt
numactl -H > $E/numactl-H.txt
cat /proc/cmdline > $E/cmdline.txt
grep -E '^(PRETTY_NAME|VERSION_ID)' /etc/os-release > $E/os-release.txt
ldd --version | head -1 > $E/glibc.txt
which taskset numactl > $E/tools.txt
taskset --version >> $E/tools.txt
numactl --version 2>&1 | head -1 >> $E/tools.txt
