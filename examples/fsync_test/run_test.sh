#!/bin/sh

file_size=16 #GB
block_size=4 #KB
fsync_period=1 
OUTPUT_PATH=result

if [ ! -d $OUTPUT_PATH ] ; then
    mkdir -p $OUTPUT_PATH
fi

#for coremask in 254
for coremask in 2 6 14 30 62 126 254
do
	str="sudo ./fsync_test -c $coremask -a fsync -F $((file_size * 1024)) -B $block_size -S $fsync_period | tee ${OUTPUT_PATH}/result_core_mask_${coremask}.txt"
	echo $str
    eval $str
done
