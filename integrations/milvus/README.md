# Milvus Knowhere graph reader

Pinned consumer: Milvus 2.6.4 and Knowhere v2.6.4 source
`500f4a89338e66086cd8c9f687ac8da459f34b95`. The original Milvus image digest is in
`../versions.json`. Knowhere's vendored DiskANN is not interchangeable with
upstream diskannpy 0.7.0 even though this reader supplies shared entry points.

Build using the integration index and follow the shared
[graph import procedure](../common/README.md#import-an-immutable-graph).
Check the actual loaded `libknowhere.so`:

```sh
python3 tools/check_reader_symbols.py "$KNOWHERE_LIBRARY"
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
close readers, then run `tools/shutdown_reader.py` and require result 0. Only
then stop Milvus. Its Go exit path may skip C++ destructors; process exit alone
is not a clean-unmount protocol. Check the actual RPC recall and a new-process
reopen before user delivery. Reconstruct host-specific container launch details
from the user's environment rather than copying historical lab scripts.

Run tool commands from `integrations/` (set `ADAPTER_DIR` to that directory).
Build from the repository root with `make -C integrations milvus SPDK_ROOT_DIR="$SPDK_SOURCE"`
after building the core. The reader source is shared in `common/`; its new
layout still requires Linux build/load and application qualification.
