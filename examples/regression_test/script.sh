#!/bin/sh

. ../root_check.sh

# MAX Test
#sudo ./regression_test -T 1 -c 2 -a regression

# Quick Test
#sudo ./regression_test -T 2 -c 2 -a regression

#if [ ! -d result ] ; then
#    mkdir -p result
#if
mkdir -p result

# Mill Test
for core_mask in 254 126 62 30 14 6 2
#for core_mask in 2 6 14 30 62 126 254
do
	str="./regression_test -T 3 -c $core_mask -a regression | tee result/result_core_mask_${core_mask}.txt"
    echo $str
    eval $str
done
