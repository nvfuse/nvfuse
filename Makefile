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

include nvfuse.mk
include spdk_config.mk

LIB_NVFUSE = nvfuse.a
SRCS   = nvfuse_buffer_cache.o nvfuse_inode_cache.o \
nvfuse_core.o nvfuse_gettimeofday.o \
nvfuse_bp_tree.o nvfuse_dirhash.o \
nvfuse_misc.o nvfuse_mkfs.o nvfuse_malloc.o nvfuse_indirect.o \
nvfuse_api.o nvfuse_aio.o \
rbtree.o \
nvfuse_ipc_ring.o nvfuse_control_plane.o \
nvfuse_dep.o nvfuse_flushwork.o \
nvfuse_reactor.o nvfuse_xattr.o

LDFLAGS += -lm -lpthread -laio -lrt -luuid
CFLAGS = $(SPDK_CFLAGS) -Iinclude -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
CFLAGS += -march=native -m64
CFLAGS += $(WARNING_OPTION) -fPIC


LEGACY_BUILD = build/legacy
OBJS=$(addprefix $(LEGACY_BUILD)/,$(SRCS))

CC=gcc

.SUFFIXES: .c .o

# .PHONY: all clean

$(LEGACY_BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "Compiling $< ..."
	@$(RM) $@
	$(CC) $(OPTIMIZATION) $(CEPH_COMPILE) $(DEBUG) -MMD -MP -c -D_GNU_SOURCE $(CFLAGS) -o $@ -ldl $<

all:  $(LIB_NVFUSE) xattr_test reactor helloworld libfuse regression_test perf control_plane_proc fsync_test create_1m_files mkfs #fio_plugin 

$(LIB_NVFUSE)	:	$(OBJS)
	$(AR) rcv $@ $(OBJS)

fio_plugin:
	make -C examples/fio_plugin

reactor:
	make -C examples/reactor

helloworld:
	make -C examples/helloworld

libfuse:
	make -C examples/libfuse

regression_test:
	make -C examples/regression_test

fsync_test:
	make -C examples/fsync_test

mkfs:
	make -C examples/mkfs

xattr_test:
	make -C examples/xattr_test

create_1m_files:
	make -C examples/create_1m_files

perf:
	make -C examples/perf

control_plane_proc:
	make -C examples/control_plane_proc

clean:
	rm -rf $(LEGACY_BUILD)
	rm -f *.o *.a *~ $(LIB_NVFUSE)
	make -C examples/fio_plugin/ clean
	make -C examples/reactor/ clean
	make -C examples/helloworld/ clean
	make -C examples/libfuse/ clean
	make -C examples/regression_test/ clean
	make -C examples/create_1m_files/ clean
	make -C examples/fsync_test/ clean
	make -C examples/perf/ clean
	make -C examples/control_plane_proc/ clean
	make -C examples/mkfs/ clean
	make -C examples/xattr_test/ clean	

distclean:
	rm -rf $(LEGACY_BUILD)
	rm -f Makefile.bak *.o *.a *~ .depend $(LIB_NVFUSE)
install: 
	chmod 755 $(LIB_NVFUSE)
	mkdir -p ../bin/
	cp -p $(LIB_NVFUSE) /usr/bin/
uninstall:
	rm -f ../bin/$(LIB_NVFUSE)
dep:    depend

depend: $(OBJS)

#
# include dependency files if they exist
#
-include $(OBJS:.o=.d)


# Native file-image development targets, separate from the SPDK build.
.PHONY: fuse fuse-adapter-test fuse-test fuse-extended-test
fuse fuse-adapter-test fuse-test fuse-extended-test:
	$(MAKE) -f Makefile.fuse $@

.PHONY: fuse-spdk
fuse-spdk:
	$(MAKE) -C examples/fuse3 -f Makefile.spdk

REGRESSION_REPEAT ?= 3
.PHONY: backend-regression backend-regression-qemu
backend-regression:
	python3 tests/run_backend_regression.py --profile portable --repeat "$(REGRESSION_REPEAT)" --sanitize
backend-regression-qemu:
	python3 scripts/test-fuse-qemu.py --backends --repeat "$(REGRESSION_REPEAT)" --sanitize

.PHONY: local-roadmap-test local local-test local-sanitize local-framework local-journal-test local-limit-test local-boundary-test local-stress-test local-resilience-test local-chaos-test
local:
	$(MAKE) -f Makefile.local all
local-roadmap-test:
	$(MAKE) -f Makefile.local roadmap-test

local-test:
	$(MAKE) -f Makefile.local test
local-resilience-test:
	$(MAKE) -f Makefile.local resilience-test
local-chaos-test:
	$(MAKE) -f Makefile.local chaos-test
local-stress-test:
	$(MAKE) -f Makefile.local stress-test
local-boundary-test:
	$(MAKE) -f Makefile.local boundary-suite
local-limit-test:
	$(MAKE) -f Makefile.local limit-test
local-journal-test:
	$(MAKE) -f Makefile.local journal-test
local-framework:
	$(MAKE) -f Makefile.local framework-test
local-sanitize:
	UBSAN_OPTIONS=halt_on_error=1 $(MAKE) -f Makefile.local LOCAL_BUILD=build/asan LOCAL_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" test journal-test limit-test boundary-suite stress-test resilience-test

# Current SPDK primary reactor and FIO API engine (separate object directory).
.PHONY: spdk-primary spdk-fio
spdk-primary:
	$(MAKE) -f Makefile.spdk all
spdk-fio:
	$(MAKE) -f Makefile.spdk fio
