#/bin/sh

make clean
make SPDK_ROOT_DIR=~/spdk/ DPDK_DIR=~/spdk/dpdk-16.11/x86_64-native-linuxapp-gcc/
