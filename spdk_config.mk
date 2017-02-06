DPDK_DIR = /home/son/nvfuse/dpdk-16.11/x86_64-native-linuxapp-gcc/
SPDK_ROOT_DIR = /home/son/nvfuse/spdk/

ifneq "$(wildcard $(SPDK_ROOT_DIR) )" ""
ifneq "$(wildcard $(DPDK_DIR) )" ""

    SPDK_LIB_LIST = nvme event log util

    include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
    include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
    include $(SPDK_ROOT_DIR)/lib/env_dpdk/env.mk
		
    LIBS += $(SPDK_LIB_LINKER_ARGS) $(ENV_LINKER_ARGS)
    SPDK_CFLAGS += $(DPDK_INC) -I$(SPDK_ROOT_DIR)/include -DSPDK_ENABLED

endif
endif
