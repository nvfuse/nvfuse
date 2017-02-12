#!/bin/sh

# MAX Test
#sudo ./regression_test -T 1 -c 2 -a regression

# Quick Test
#sudo ./regression_test -T 2 -c 2 -a regression

# Mill Test
for core_mask in 126 62 30 14 6 2
#for core_mask in 6
do
	sudo ./regression_test -T 3 -c $core_mask -a regression > result/result_core_mask_${core_mask}.txt
done
