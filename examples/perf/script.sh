#!/bin/sh

echo "10G file size, 4K block size, libaio, 128 qdepth, random workload, direct I/O, write workload"
sudo ./perf -t spdk -d 01:00 -f -m -S 10240 -B 4096 -E libaio -Q 128 -R -D -W

echo "10G file size, 4K block size, libaio, 128 qdepth, random workload, direct I/O, read workload"
sudo ./perf -t spdk -d 01:00 -f -m -S 10240 -B 4096 -E libaio -Q 128 -R -D
