#!/bin/bash
C=$CAMPAIGNS/2026-11-04-pub3-hp
$C/scripts/sl.sh > $C/sl.log 2>&1
$C/scripts/long.sh > $C/long.log 2>&1
echo "$(date -Is) all long done hugepages=$(grep -E 'HugePages_(Total|Free)' /proc/meminfo | tr -s ' ' | tr '\n' ' ')" >> $C/progress.log
