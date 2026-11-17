NVFUSE_ROOT_DIR ?= $(abspath ..)
SPDK_ROOT_DIR ?= $(HOME)/src/spdk
FIO_SOURCE_DIR ?= $(HOME)/src/fio
NVFUSE_ARCHIVE ?= $(NVFUSE_ROOT_DIR)/build/spdk-primary/nvfuse.a
CXX ?= g++
.DEFAULT_GOAL := help
.PHONY: help check native graph fio leveldb rocksdb lmcache ann milvus
help:
	@printf '%s\n' 'Optional integrations; core builds independently.' 'Targets: fio leveldb rocksdb lmcache ann milvus' 'Supply SPDK_ROOT_DIR and the documented application source/library paths.' 'These targets build sources only; they never provision devices or run workloads.'
check:
	@test "$$(uname -s)" = Linux && test "$$(uname -m)" = x86_64 || { echo 'Linux x86_64 is required' >&2; exit 1; }
	@test -f "$(NVFUSE_ARCHIVE)" || { echo 'Build the matching core archive first' >&2; exit 1; }
native: check
	$(MAKE) -C common -f Makefile.native NVFUSE_ROOT_DIR="$(NVFUSE_ROOT_DIR)" SPDK_ROOT_DIR="$(SPDK_ROOT_DIR)" NVFUSE_ARCHIVE="$(NVFUSE_ARCHIVE)"
graph: native
	$(CXX) -std=c++17 -O3 -g -fPIC -shared common/graph_reader.cpp -Lcommon -lnvfuse_ai -Wl,-rpath,'$$ORIGIN' -pthread -o common/libnvfuse_diskann.so
ann milvus: graph
lmcache: native
fio:
	$(MAKE) -C fio NVFUSE_ROOT_DIR="$(NVFUSE_ROOT_DIR)" SPDK_ROOT_DIR="$(SPDK_ROOT_DIR)" NVFUSE_ARCHIVE="$(NVFUSE_ARCHIVE)" FIO_SOURCE_DIR="$(FIO_SOURCE_DIR)"
leveldb rocksdb:
	$(MAKE) -C $@ nvfuse NVFUSE_ROOT_DIR="$(NVFUSE_ROOT_DIR)" SPDK_ROOT_DIR="$(SPDK_ROOT_DIR)" NVFUSE_ARCHIVE="$(NVFUSE_ARCHIVE)" DB_SOURCE_DIR="$(DB_SOURCE_DIR)" DB_LIBRARY="$(DB_LIBRARY)" DB_EXTRA_LIBS="$(DB_EXTRA_LIBS)"
