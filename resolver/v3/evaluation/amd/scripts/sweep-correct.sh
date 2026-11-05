#!/bin/bash
E=$EVAL; R=$E/runs/correct; mkdir -p $R
for cfg in 2x8 2x10 2x12 3x12 3x20; do for spec in assert:points:60 assert:ranges:60 assert:points:3000 assert:ranges:3000 gate:points:60 gate:ranges:60 gate:points:3000 gate:ranges:3000; do
IFS=: read k w n <<< "$spec"; B=$E/bin/blocksizebench-$k-v38-$cfg; o=$R/C-$k-$cfg-$w-$n
docker run --rm --log-driver=none --cpuset-cpus=7 --cpuset-mems=0 -v $E:$E foundationdb/build:rockylinux9-latest bash -c "export LD_LIBRARY_PATH=\$(dirname \$(/usr/local/bin/clang++ -stdlib=libc++ -print-file-name=libc++.so.1)); timeout 900 $B $w $n 1024 > $o.txt 2> $o.err; echo \$? > $o.rc"
echo "$cfg $k $w $n rc=$(cat $o.rc) $(grep -oE 'conflicts=[0-9]+ reclaimCV=[0-9]+ decisions=[0-9a-f]+' $o.txt)"
done; done
