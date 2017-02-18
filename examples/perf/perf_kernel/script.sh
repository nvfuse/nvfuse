#!/bin/sh

for i in {2..6}
do
    echo start $i
    ./perf_kernel -M stat -C 1000000 -T $i
    echo end $i
    echo ""
done
