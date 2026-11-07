#!/bin/bash
set -u
C=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source <(sed '/^echo "campaign start/,$d' "$C/scripts/campaign.sh")
echo "replicates start $(date -Is)" >> $LOG
measured m3-replicate
measured m4-replicate
measured m5-replicate
echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null
echo "replicates done $(date -Is)" >> $LOG
