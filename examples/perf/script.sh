#!/bin/sh

file_size=128 #giga
for qdepth in 1 2 4 8 16 32 64 128 256
do
	echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, random workload, direct I/O, write workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -R -D -W > tmp/4K_write_${qdepth}_qdepth_${file_size}G_random.log

	echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, random workload, direct I/O, read workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -R -D > tmp/4K_read_${qdepth}_qdepth_${file_size}G_random.log
done

file_size=128 #giga
for qdepth in 1 2 4 8 16 32 64 128 256
do
	echo "${file_size}G file size, 128K block size, libaio, $qdepth qdepth, random workload, direct I/O, write workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $((128*1024)) -E libaio -Q $qdepth -R -D -W > tmp/128K_write_${qdepth}_qdepth_${file_size}G_random.log

	echo "${file_size}G file size, 128K block size, libaio, $qdepth qdepth, random workload, direct I/O, read workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $((128*1024)) -E libaio -Q $qdepth -R -D > tmp/128K_read_${qdepth}_qdepth_${file_size}G_random.log
done

file_size=128 #giga
for qdepth in 1 2 4 8 16 32 64 128 256
do
	echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, seq workload, direct I/O, write workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -D -W > tmp/4K_write_${qdepth}_qdepth_${file_size}G_seq.log

	echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, seq workload, direct I/O, read workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -D > tmp/4K_read_${qdepth}_qdepth_${file_size}G_seq.log
done

file_size=128 #giga
for qdepth in 1 2 4 8 16 32 64 128 256
do
	echo "${file_size}G file size, 128K block size, libaio, $qdepth qdepth, seq workload, direct I/O, write workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $((128*1024)) -E libaio -Q $qdepth -D -W > tmp/128K_write_${qdepth}_qdepth_${file_size}G_seq.log

	echo "${file_size}G file size, 128K block size, libaio, $qdepth qdepth, seq workload, direct I/O, read workload"
	sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $((128*1024)) -E libaio -Q $qdepth -D > tmp/128K_read_${qdepth}_qdepth_${file_size}G_seq.log
done
