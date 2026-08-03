#!/bin/bash
SP="$1"; cd /home/snokvist/dev/waybeam-coordination/waybeam_venc
: > $SP/sweep/results3.txt
for cfg in "8 1" "4 5" "3 5" "2 5" "2 10"; do
  set -- $cfg
  echo "### slots=$1 ai_every=$2" >> $SP/sweep/results3.txt
  bash $SP/sweep/run2.sh $1 $2 4 40 8000 >> $SP/sweep/results3.txt 2>&1
done
echo "SWEEP_DONE" >> $SP/sweep/results3.txt
