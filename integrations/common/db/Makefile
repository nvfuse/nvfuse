# Included by the LevelDB and RocksDB integration entry points.
NVFUSE_ARCHIVE ?= $(NVFUSE_ROOT_DIR)/build/spdk-primary/nvfuse.a
SPDK_ROOT_DIR ?= $(HOME)/src/spdk
DB_SOURCE_DIR ?=
DB_LIBRARY ?=
DB_EXTRA_LIBS ?=
CXX ?= g++
ADAPTER_SOURCE_DIR := $(INTEGRATIONS_DIR)/common/db
BUILD_DIR ?= $(NVFUSE_ROOT_DIR)/build/integrations/$(DB)
APP_FLAGS = -O2 -std=c++17 -fno-rtti -include cstdint -I$(ADAPTER_SOURCE_DIR) -I$(DB_SOURCE_DIR)/include
ifeq ($(DB),rocksdb)
APP_FLAGS += -DKV_ROCKS
else
APP_FLAGS += -I$(DB_SOURCE_DIR)
endif
.DEFAULT_GOAL := help
.PHONY: help check-db check-core nvfuse posix
help:
	@printf '%s\n' 'Targets: nvfuse posix' 'Required: DB_SOURCE_DIR=/path/to/source DB_LIBRARY=/path/to/static/library' 'Native also requires NVFUSE_ROOT_DIR and SPDK_ROOT_DIR. See README.md.'
check-db:
	@test -f "$(DB_SOURCE_DIR)/include/$(DB)/db.h" || { echo 'Set DB_SOURCE_DIR to the documented upstream source' >&2; exit 1; }
	@test -f "$(DB_LIBRARY)" || { echo 'Set DB_LIBRARY to its built static library' >&2; exit 1; }
check-core:
	@test -f "$(NVFUSE_ARCHIVE)" || { echo 'Build the matching core archive first' >&2; exit 1; }
	@test -f "$(SPDK_ROOT_DIR)/mk/spdk.common.mk" || { echo 'Set SPDK_ROOT_DIR to the matching SPDK build' >&2; exit 1; }
nvfuse: check-db check-core
	mkdir -p "$(BUILD_DIR)/native"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(APP_FLAGS) -DKV_NV -c "$(ADAPTER_SOURCE_DIR)/kv_app.cc" -o "$(BUILD_DIR)/native/app.o"
	cp "$(ADAPTER_SOURCE_DIR)/nvbridge.c" "$(ADAPTER_SOURCE_DIR)/nvbridge.h" "$(BUILD_DIR)/native/"
	$(MAKE) -C "$(BUILD_DIR)/native" -f "$(ADAPTER_SOURCE_DIR)/Makefile.spdk" NVFUSE_ROOT_DIR="$(NVFUSE_ROOT_DIR)" SPDK_ROOT_DIR="$(SPDK_ROOT_DIR)" NVFUSE_ARCHIVE="$(NVFUSE_ARCHIVE)" DB_LIBRARY="$(DB_LIBRARY)" DB_EXTRA_LIBS="$(DB_EXTRA_LIBS)"
posix: check-db
	mkdir -p "$(BUILD_DIR)/posix"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(APP_FLAGS) "$(ADAPTER_SOURCE_DIR)/kv_app.cc" "$(DB_LIBRARY)" $(DB_EXTRA_LIBS) -lpthread -ldl -lm -o "$(BUILD_DIR)/posix/kv_app"
