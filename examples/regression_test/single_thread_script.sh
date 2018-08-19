#!/bin/sh

. ../root_check.sh

mkdir -p result

# -T tes type 
#-c core mask 
#-a appplication name 
#-f mkfs file system format
#-m file system mount

# 2 CPU core
core_mask=3

str="./regression_test -T 3 -c $core_mask -a regression -f -m -o nvme.conf | tee result/result_core_mask_${core_mask}.txt"
echo $str
eval $str
