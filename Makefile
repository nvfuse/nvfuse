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

include spdk_config.mk

LIB_NVFUSE = nvfuse.a
SRCS   = nvfuse_buffer_cache.o \
nvfuse_core.o nvfuse_unix_io.o nvfuse_gettimeofday.o \
nvfuse_bp_tree.o nvfuse_dirhash.o \
nvfuse_misc.o nvfuse_mkfs.o nvfuse_malloc.o nvfuse_indirect.o \
nvfuse_spdk.o nvfuse_file_io.o nvfuse_ramdisk_io.o \
nvfuse_api.o nvfuse_aio.o \
rbtree.o

LDFLAGS += -lm -lpthread -laio -lrt
CFLAGS = $(SPDK_CFLAGS) -Iinclude -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
#CFLAGS = -Iinclude -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
CFLAGS += -m64

OBJS=$(SRCS:.c=.o)

CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

.c.o:
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC) -O2 -g -c -D_GNU_SOURCE $(CFLAGS) -o $@ $<

all:  $(LIB_NVFUSE) helloworld nvfuse_cli libfuse regression_test

$(LIB_NVFUSE)	:	$(OBJS)
	$(AR) rcv $@ $(OBJS)

helloworld:
	make -C examples/helloworld

nvfuse_cli:
	make -C examples/nvfuse_cli

libfuse:
	make -C examples/libfuse

regression_test:
	make -C examples/regression_test

clean:
	rm -f *.o *.a *~ $(LIB_NVFUSE)
	make -C examples/helloworld/ clean
	make -C examples/nvfuse_cli/ clean
	make -C examples/libfuse/ clean
	make -C examples/regression_test/ clean

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

