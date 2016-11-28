#
#	NVFUSE (NVMe based File System in Userspace)
#	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
#	First Writing: 30/10/2016
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#

#DPDK_DIR = /root/spdk/dpdk-16.07/x86_64-native-linuxapp-gcc/
#SPDK_ROOT_DIR = /root/spdk

ifneq "$(wildcard $(SPDK_ROOT_DIR) )" ""
ifneq "$(wildcard $(DPDK_DIR) )" ""

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
SPDK_LIBS += $(SPDK_ROOT_DIR)/lib/nvme/libspdk_nvme.a \
	     $(SPDK_ROOT_DIR)/lib/util/libspdk_util.a \
	     $(SPDK_ROOT_DIR)/lib/memory/libspdk_memory.a \
	     $(SPDK_ROOT_DIR)/lib/log/libspdk_log.a \

LIBS += $(SPDK_LIBS) $(PCIACCESS_LIB) $(DPDK_LIB)
SPDK_CFLAGS := $(DPDK_INC) -I$(SPDK_ROOT_DIR)/include -DSPDK_ENABLED

endif
endif


LIB_NVFUSE = nvfuse.a
SRCS   = nvfuse_buffer_cache.o \
nvfuse_core.o nvfuse_unix_io.o nvfuse_gettimeofday.o \
nvfuse_bp_tree.o nvfuse_dirhash.o \
nvfuse_misc.o nvfuse_mkfs.o nvfuse_malloc.o nvfuse_indirect.o \
nvfuse_spdk.o \
nvfuse_api.o nvfuse_aio.o \
rbtree.o

LDFLAGS := -lm -lpthread -laio -lrt
CFLAGS := $(SPDK_CFLAGS) -Iinclude -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE

OBJS=$(SRCS:.c=.o)

CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

.c.o:
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC)  -pg -g -c -D_GNU_SOURCE $(CFLAGS) -o $@ $<

all:  $(LIB_NVFUSE) helloworld nvfuse_cli libfuse

$(LIB_NVFUSE)	:	$(OBJS)
	$(AR) rcv $@ $(OBJS)

helloworld:
	make -C examples/helloworld

nvfuse_cli:
	make -C examples/nvfuse_cli

libfuse:
	make -C examples/libfuse
clean:
	rm -f *.o *.a *~ $(LIB_NVFUSE)
	make -C examples/helloworld/ clean
	make -C examples/nvfuse_cli/ clean
	make -C examples/libfuse/ clean

distclean:
	rm -f Makefile.bak *.o *.a *~ .depend $(LIB_NVFUSE)
install: 
	chmod 755 $(LIB_NVFUSE)
	mkdir -p ../bin/
	cp -p $(LIB_NVFUSE) /usr/bin/
uninstall:
	rm -f ../bin/$(LIB_NVFUSE)
dep:    depend

depend:
	$(CC) -MM $(CFLAGS) $(SRCS) 1>.depend

#
# include dependency files if they exist
#
ifneq ($(wildcard .depend),)
include .depend
endif

