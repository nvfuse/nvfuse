#
#	NVFUSE (NVMe based File System in Userspace)
#	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
#	First Writing: 03/01/2017
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

NVFUSE_ROOT_DIR := $(abspath $(CURDIR)/../../../)
NVFUSE_LIBS := $(NVFUSE_ROOT_DIR)/nvfuse.a

include $(NVFUSE_ROOT_DIR)/spdk_config.mk

TARGET_KERNEL = kernel_fs

SRCS_KERNEL = perf_kernel.o

LDFLAGS += -lm -lpthread -laio -lrt
LDFLAGS_KERNEL = -lpthread

CFLAGS += $(SPDK_CFLAGS) -I$(NVFUSE_ROOT_DIR)/include -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE
CFLAGS += -w

OBJS_KERNEL=$(SRCS_KERNEL:.c=.o)


CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

.c.o:
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC) -O2 -g -c -D_GNU_SOURCE $(CFLAGS) -o $@ $<

$(TARGET_KERNEL)	:	$(OBJS_KERNEL)
	$(CC) -g -o $(TARGET_KERNEL) $(OBJS_KERNEL) $(LDFLAGS)

all:  $(TARGET_KERNEL)


clean:
	rm -f *.o *.a *~ $(TARGET_KERNEL)


