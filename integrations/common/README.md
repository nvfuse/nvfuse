# Shared adapter components

`native_io.c` provides bounded reusable DMA buffers and client contexts for the
LMCache and graph-reader adapters. `graph_reader.cpp` supplies the version-pinned
DiskANN/Knowhere ELF entry points. They link the core archive; they do not include
or replace `nvfuse_spdk_runtime.c`. Build from the repository root with:

```sh
make -C integrations lmcache SPDK_ROOT_DIR="$SPDK_SOURCE"
make -C integrations ann SPDK_ROOT_DIR="$SPDK_SOURCE"
```

Outputs remain beside these sources and are ignored. The C bridge also retains
XFS batching helpers for the LMCache reference path. The bridge selects embedded
signals and socket-any IPC allocation; verify actual NUMA placement. Default
wait policy is pause and queues per context are four. The reader defaults to 32
contexts; every context may reserve 8MiB DMA inside the shared 2GiB SPDK arena.
Keep these options fixed when comparing core versions.

The [DB reference adapter](db/README.md) is shared by LevelDB and RocksDB. Host
paths, result files, data sets and benchmark summaries do not belong here.

## Import an immutable graph

From `integrations/`, set the core runtime variables as described in the
[core guide](../../docs/user-guide.md#primary-apispdk-on-linux). Use an already
prepared, exclusively owned test volume and an absolute path for each placeholder.
`GRAPH_FILE` must be nonempty and padded to a multiple of 4096 bytes. Choose a
new NVFUSE destination such as `/graphs/demo.disk.index` and keep the original
POSIX header and auxiliary files available to the application.

```sh
mkdir -p ../build/integration-checks
python3 tools/import_graph.py \
  --library "$PWD/common/libnvfuse_ai.so" \
  --file "$GRAPH_FILE=/graphs/demo.disk.index" \
  --output ../build/integration-checks/graph-import.json
```

This writes the selected destination and verifies every byte by reading it back.
It does not format unless the separate `--format` option is explicitly supplied.
Finish the import before starting a reader process. Copy the selected application's
`routes.example.tsv` into an ignored private configuration directory and replace
both placeholder fields: the POSIX suffix must uniquely identify the graph, and
the NVFUSE path must equal its imported destination. Keep traces and socket paths
private to the owned process. Continue with the [DiskANN](../ann/README.md) or
[Milvus](../milvus/README.md) guide for reader loading and clean shutdown.
