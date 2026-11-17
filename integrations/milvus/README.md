# Milvus Knowhere graph reader

Pinned consumer: Milvus 2.6.4 and Knowhere v2.6.4 source
`500f4a89338e66086cd8c9f687ac8da459f34b95`. The original Milvus image digest is in
`../versions.json`. Knowhere's vendored DiskANN is not interchangeable with
upstream diskannpy 0.7.0 even though this reader supplies shared entry points.

**Experimental, version-pinned reference adapter.** The pinned Linux standalone
fixture passes POSIX insert/index/search, offline graph import, native RPC recall,
explicit reader/runtime shutdown and new-server reopen. These are focused
correctness checks; production deployment, saturation and long-running workloads
require their own qualification. The commands below reproduce the checks; a
successful build is insufficient.

The shared bridge allocates reusable DMA slots on demand. Reserving 8 MiB for
every search thread previously exhausted the shared SPDK arena; allocation
exceptions left DiskANN queries waiting for scratch resources, followed by
`-EBUSY` on shutdown. Small graph reads now reserve only the slot sizes they use.
The [DMA regression](../common/README.md#dma-buffer-regression) checks multiple
contexts, payloads, buffer growth and draining accepted I/O after allocation
failure. Keep context limits and the shared arena budget appropriate to the
target workload; lazy allocation does not make memory unlimited.

Build using the integration index and follow the shared
[graph import procedure](../common/README.md#import-an-immutable-graph).
Check the actual loaded `libknowhere.so`:

```sh
python3 integrations/tools/check_reader_symbols.py "$KNOWHERE_LIBRARY"
```

For a controlled single-owner standalone instance, locate each staged immutable
graph, import and fully read it back, and assign one unique route per graph.
`milvus/routes.example.tsv` is a placeholder, not a collection discovery tool.
Retain Milvus's original graph headers, PQ/auxiliary files and staging paths.

Set these environment variables on the owned application process:

| Variable | Value |
| --- | --- |
| `NVAI_READER_MAP` | Absolute path to the reviewed route TSV |
| `NVAI_READER_EVIDENCE` | New private reader JSONL trace path |
| `NVAI_READER_CONTROL` | New short private UNIX socket path |
| `LD_PRELOAD` | Absolute path to `common/libnvfuse_diskann.so` |

The socket path must be new, private and shorter than the Linux UNIX-path limit.
Apply these variables only to the owned reader process, not a whole login session
or unrelated worker processes. The runtime starts lazily on the first graph open.
Do not run offline import concurrently with the serving process.

Pass these variables and both shared libraries into the actual Milvus server
process/container; host environment variables alone do not enter a container.
Container paths, SPDK device access, hugepages, bdev JSON and route destinations
must agree. Keep unrelated services outside `LD_PRELOAD`. A production
multi-query-node deployment needs explicit volume ownership before use.

This integration redirects only aligned graph-sector reads. WAL, object storage,
metadata, index construction and other PQ/staging operations retain their
ordinary storage. It is not a full Milvus storage backend or official plugin.

On shutdown, stop new RPCs, finish accepted queries, release the collection and
close readers, then run `integrations/tools/shutdown_reader.py` and require result 0. Only
then stop Milvus. Its Go exit path may skip C++ destructors; process exit alone
is not a clean-unmount protocol. Check the actual RPC recall and a new-process
reopen before user delivery. Reconstruct host-specific container launch details
from the user's environment rather than copying historical lab scripts.

Run the commands on this page from the repository root.
Build from the repository root with `make -C integrations milvus SPDK_ROOT_DIR="$SPDK_SOURCE"`
after building the core. The reader source is shared in `common/`; qualify the
resulting library against the pinned application before use.

## Reproducible RPC fixture

Install the pinned Milvus distribution/image and `pymilvus==2.6.4` with
`numpy==1.25.0` in a Python 3.11 environment. The server and its original shared
libraries must run inside their matching distribution/container filesystem.
An image's ELF loader alone does not make its libraries compatible with the
host's libc. Build the NVFUSE reader for the server's environment or provide its
matching runtime dependencies there, and check the actual Knowhere symbols.

`configure.py` copies the pinned distribution's base configuration and writes
private standalone overrides, local storage, Rocksmq and an external etcd
configuration.
It refuses an existing directory. Use the official etcd v3.5.18 release and verify
its archive against the published SHA256SUMS. Select unoccupied ports and use a dedicated
standalone test instance:

```sh
python3 integrations/milvus/configure.py \
  --upstream-config "$MILVUS_CONFIG_SOURCE" --directory build/milvus-demo
export MILVUSCONF="$PWD/build/milvus-demo/configs"
export DEPLOY_MODE=STANDALONE
# Start etcd v3.5.18 in a separate shell; keep it running across Milvus restarts.
"$ETCD_BIN" --config-file "$MILVUSCONF/etcd.yaml"
# Start the pinned server using its distribution/container entry point.
# Mount the configuration and data at the same absolute paths inside a container.
"$MILVUS_BIN" run standalone
```

Run the client from another shell in the repository root. Keep its environment
free of the server's `LD_PRELOAD`. Set `MILVUS_PY` to that Python environment:

```sh
"$MILVUS_PY" integrations/milvus/smoke.py prepare --keep-loaded \
  --uri http://127.0.0.1:19536 --collection nvfuse_release_demo \
  --directory build/milvus-fixture \
  --output build/integration-checks/milvus-posix.json
```

The client inserts 10,000 64-dimensional vectors, flushes, creates a real DISKANN
index, loads it and verifies 16 RPC queries against exact top-10 ground truth.
Recall@10 must reach 0.95. A new fixture directory and collection name are
required. `--keep-loaded` leaves the prepared collection loaded for graph
inspection; otherwise the client releases it before closing.

While the private server is idle, locate its staged `*_disk.index` files under
`build/milvus-demo/data/local/`. Copy each immutable graph into ignored `build/`
and record its full-byte hash, original staged path and a unique route suffix.
Then release the collection from a client before stopping that server:

```sh
"$MILVUS_PY" - <<'PYCODE'
from pymilvus import MilvusClient
client = MilvusClient(uri="http://127.0.0.1:19536", timeout=30)
client.release_collection("nvfuse_release_demo", timeout=30)
client.close()
PYCODE
```

Stop the server with SIGTERM and require exit status 0. Do not import while any
process owns the NVFUSE volume. Import every saved graph with
`integrations/tools/import_graph.py` and create a reviewed TSV as described in
[the common guide](../common/README.md#import-an-immutable-graph). Use distinct
root-level NVFUSE destination paths; nonexistent parent directories are not
created by the importer. Retain original POSIX graph headers and auxiliary files.

Restart the same standalone server with the reader/runtime variables above.
Mount both adapter shared libraries, the bdev JSON, route file, data and required
SPDK resources into the server's filesystem at matching paths. Use new evidence
and control-socket paths. Then run:

```sh
"$MILVUS_PY" integrations/milvus/smoke.py verify \
  --uri http://127.0.0.1:19536 --collection nvfuse_release_demo \
  --directory build/milvus-fixture \
  --reader-evidence "$READER_TRACE" \
  --output build/integration-checks/milvus-native.json
python3 integrations/tools/shutdown_reader.py "$READER_SOCKET"
```

The verifier bounds RPC calls (60 seconds by default) and collection release
(30 seconds); `--timeout` changes the RPC budget. A timeout is a failed check.
Require recall success and positive native graph-read/close evidence, followed
by shutdown result 0 before stopping the server. Repeat with a new server
process and new trace/socket/output files for a durable reopen check. Keep
failed data and logs for diagnosis. These are correctness checks; Milvus WAL,
object storage, index build and auxiliary files retain their ordinary storage.

Stop etcd with SIGTERM only after the final Milvus server has exited. Require
exit status 0 from the verifier, NVFUSE shutdown tool and Milvus server. Official
etcd 3.5.18 handles SIGTERM, logs `closed etcd server`, then preserves the signal
exit status (143 in a shell, -15 in Python); require that final close record and
no remaining etcd process. Retain failure logs. Milvus 2.6.4's
embedded-etcd shutdown produced a `CloseEtcdClient` panic in a standalone
POSIX-only check, so this example uses an independent etcd process. That
upstream shutdown failure must not be counted as a successful release check.
