SPDK_ROOT_DIR ?= $(HOME)/src/spdk
FIO_SOURCE_DIR ?= $(HOME)/src/fio
NVFUSE_ROOT_DIR := $(abspath ../../..)
NVFUSE_ARCHIVE ?= $(NVFUSE_ROOT_DIR)/build/spdk-primary/nvfuse.a
APP = nvfuse_spdk.so
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
C_SRCS = fio_nvfuse.c runtime.c
CFLAGS += -fPIC -DDEBUG -DSPDK_ENABLED -DNVFUSE_PRIMARY_JOURNAL -I$(SPDK_ROOT_DIR)/dpdk/build/include -D_GNU_SOURCE -I$(NVFUSE_ROOT_DIR)/include -I$(FIO_SOURCE_DIR)
LDFLAGS += -shared -Wl,--version-script=exports.map
LIBS += $(NVFUSE_ARCHIVE) -lm -lpthread -laio -luuid
SPDK_LIB_LIST += event $(EVENT_BDEV_SUBSYSTEM) $(KEYRING_MODULES_LIST) $(BLOCKDEV_MODULES_LIST) $(SOCK_MODULES_LIST) bdev
include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
$(APP): Makefile exports.map $(NVFUSE_ARCHIVE)
runtime.o: cache_policy.h spsc.h stats.h completion.h api_queue.h buffered_wb.inc
