#!/bin/sh

file_size=16 #GB
block_size=4 #KB
fsync_period=1 

#for coremask in 2 6 14
for coremask in 14
do
	str="sudo ./fsync_test -c $coremask -a fsync -F $((file_size * 1024)) -B $block_size -S $fsync_period"
	echo $str
	eval $str
done
