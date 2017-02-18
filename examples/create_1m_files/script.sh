#!/bin/sh

mkdir -p result

# Mill Test
for core_mask in 254 126 62 30 14 6 2
do
	str="sudo ./create_1m_files -c $core_mask -a test | tee result/result_core_mask_${core_mask}.txt"
	echo $str
	eval $str
done
