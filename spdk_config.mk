#DPDK_DIR = /home/foo/spdk/dpdk-16.11/x86_64-native-linuxapp-gcc/
#SPDK_ROOT_DIR = /home/foo/spdk

ifneq "$(wildcard $(SPDK_ROOT_DIR) )" ""
ifneq "$(wildcard $(DPDK_DIR) )" ""

	SPDK_LIB_LIST = nvme event log util 
	SPDK_LIB_LIST += conf bdev trace util
	SPDK_LIB_LIST += copy conf rpc jsonrpc json
	#SPDK_LIB_LIST += event_bdev
	#SPDK_LIB_LIST += lvol vbdev_lvol

	ifneq ($(CEPH_SPDK),1)
		SPDK_LIB_LIST += event_bdev event_copy event_rpc
	endif

	include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
	include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
	include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
	include $(SPDK_ROOT_DIR)/lib/env_dpdk/env.mk

	LIBS += $(BLOCKDEV_MODULES_LINKER_ARGS) \
		$(COPY_MODULES_LINKER_ARGS)
	LIBS += $(SPDK_LIB_LINKER_ARGS) $(ENV_LINKER_ARGS)

	SPDK_CFLAGS += $(DPDK_INC) -I$(SPDK_ROOT_DIR)/include -DSPDK_ENABLED -fPIC
endif
endif
