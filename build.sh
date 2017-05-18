#/bin/sh

make clean
make SPDK_ROOT_DIR=~/spdk/ DPDK_DIR=~/spdk/dpdk/x86_64-native-linuxapp-gcc/
