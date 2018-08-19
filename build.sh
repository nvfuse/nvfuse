#/bin/sh

source export.sh

#Set NVFUSE_CEPH_HOME to your ceph src directory
if [ -n "$NVFUSE_CEPH_HOME" ]; then
  make SPDK_ROOT_DIR=${NVFUSE_CEPH_HOME}/src/spdk DPDK_DIR=/usr/ DPDK_INC=-I/usr/include/dpdk CEPH_SPDK=1
else
  make SPDK_ROOT_DIR=~/spdk DPDK_DIR=~/spdk/dpdk/build
fi