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

NVFUSE_ROOT_DIR := $(abspath $(CURDIR)/../..)
NVFUSE_LIBS := $(NVFUSE_ROOT_DIR)/nvfuse.a

include $(NVFUSE_ROOT_DIR)/nvfuse.mk
include $(NVFUSE_ROOT_DIR)/spdk_config.mk

TARGET = create_1m_files
SRCS   = create_1m_files.o

LDFLAGS += -lm -lpthread -laio -lrt -luuid
CFLAGS += $(SPDK_CFLAGS) -I$(NVFUSE_ROOT_DIR)/include -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE
CFLAGS += $(WARNING_OPTION)

OBJS=$(SRCS:.c=.o)

CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

.c.o:
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC) $(OPTIMIZATION) $(CEPH_COMPILE) $(DEBUG) -c -D_GNU_SOURCE $(CFLAGS) -o $@ $<

$(TARGET)	:	$(OBJS)
	$(CC) -o $(TARGET) $(OBJS) $(NVFUSE_LIBS) $(LIBS) $(LDFLAGS)
all:  $(TARGET) 


clean:
	rm -f *.o *.a *~ $(TARGET)

distclean:
	rm -f Makefile.bak *.o *.a *~ .depend $(TARGET)
install: 
	chmod 755 $(TARGET)
uninstall:

dep:    depend

depend:

#
# include dependency files if they exist
#
ifneq ($(wildcard .depend),)
include .depend
endif

