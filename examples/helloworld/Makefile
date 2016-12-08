DPDK_DIR = /root/spdk/dpdk-16.07/x86_64-native-linuxapp-gcc/
SPDK_ROOT_DIR = /root/spdk

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

NVFUSE_ROOT_DIR := $(abspath $(CURDIR)/../..)
NVFUSE_LIBS := $(NVFUSE_ROOT_DIR)/nvfuse.a

TARGET = helloworld
SRCS   = helloworld.o

LDFLAGS := -lm -lpthread -laio -lrt
CFLAGS := $(SPDK_CFLAGS) -I$(NVFUSE_ROOT_DIR)/include -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE

OBJS=$(SRCS:.c=.o)

CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

.c.o:
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC)  -g -c -D_GNU_SOURCE $(CFLAGS) -o $@ $<

$(TARGET)	:	$(OBJS)
	$(CC)  -g -o $(TARGET) $(OBJS) $(NVFUSE_LIBS) $(LIBS) $(LDFLAGS) -D_GNU_SOURCE $(CFLAGS)

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

