
DEBUG = -g -DDEBUG
OPTIMIZATION = -O3
WARNING_OPTION = -Wall -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-sign-compare

# Please use the below definition when the spdk forked in the ceph repo is deploy
CEPH_SPDK = 0

#CEPH_SPDK = 1

ifeq ($(CEPH_SPDK),1)
	CEPH_COMPILE = -DNVFUSE_USE_CEPH_SPDK
else
endif
