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
wait policy is `block` and the queue capacity per context is four. The reader
defaults to 32 contexts. DMA slots are allocated on first use and grow to the
largest request piece seen by that slot, up to 8 MiB per context inside the shared 2 GiB SPDK
arena. A full 32-slot batch of 4 KiB graph reads needs 128 KiB, rather than
reserving 8 MiB for every search thread. Allocation failures drain accepted I/O
before returning an error.

The core also creates AIO lanes on demand up to the requested context limit.
This is separate from DMA buffer growth: a small graph batch generally needs one
core lane, while bulk I/O retains the full requested reservation capacity.

An empty completion poll uses the core `nvfio_wait()` API with a 1000-microsecond
sleep limit. This releases the application CPU while the filesystem and I/O
reactors make progress. `NVAI_WAIT_POLICY=pause` restores the previous spin loop;
`yield` uses the scheduler instead. Invalid values fail startup. The wait setting
applies to the shared NVFUSE bridge; the XFS batching helper keeps its Linux AIO
completion path. Rebuild the bridge against the matching core to use the new API.
Small 4 KiB read batches can coalesce notifications in the core; larger I/O and
writes keep first-completion notification. Signals, timeouts and already-ready
results can return partial batches, which the bridge continues to drain.

Keep these options fixed when comparing core versions.

Arena provisioning runs before requests are served. The bridge allocates at most
1 GiB at a time and explicitly fsyncs each chunk, then truncates to the requested
size and fsyncs again. The primary SPDK fallocate path does not commit on return,
and a long owner callback prevents periodic commits; bounded chunks alone would
still accumulate journal metadata. This setup rule bounds that accumulation. It
does not extend the core's arbitrary single-call fallocate transaction capacity.

The [DB reference adapter](db/README.md) is shared by LevelDB and RocksDB. Host
paths, result files, data sets and benchmark summaries do not belong here.

## DMA buffer regression

After importing an immutable graph, keep the volume exclusively owned and set
the usual core runtime variables with formatting disabled. The following check
reads an existing graph without changing its contents. Its original POSIX copy
must contain at least 512 KiB and match the imported bytes. From the repository
root, build the test-only allocator probe and run:

```sh
mkdir -p build/integration-checks
cc -std=c11 -Wall -Wextra -Werror -fPIC -shared \
  integrations/common/dma_probe.c -ldl -pthread \
  -o build/integration-checks/dma_probe.so
LD_PRELOAD="$PWD/build/integration-checks/dma_probe.so" \
python3 integrations/common/native_buffer_checks.py \
  --library "$PWD/integrations/common/libnvfuse_ai.so" \
  --graph /demo.disk.index --expected "$GRAPH_FILE" \
  --output build/integration-checks/native-buffers.json
```

This checks 12 idle contexts without DMA reservation, parallel full-payload
reads, slot growth/reuse, a 32-request batch of 256 KiB pieces and clean shutdown.
It also fails the third DMA allocation after two requests have been accepted;
the context must be immediately freeable when the call returns `-ENOMEM`.
The probe wraps the real SPDK allocator. Apply its `LD_PRELOAD` only to this
test process; it is not an application dependency or a server setting.

## Import an immutable graph

From `integrations/`, set the core runtime variables as described in the
[core guide](../../docs/user-guide.md#primary-apispdk-on-linux). Use an already
prepared, exclusively owned test volume and an absolute path for each placeholder.
`GRAPH_FILE` must be nonempty and padded to a multiple of 4096 bytes. Choose a
new NVFUSE destination such as `/demo.disk.index` and keep the original
POSIX header and auxiliary files available to the application.

```sh
mkdir -p ../build/integration-checks
python3 tools/import_graph.py \
  --library "$PWD/common/libnvfuse_ai.so" \
  --file "$GRAPH_FILE=/demo.disk.index" \
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
