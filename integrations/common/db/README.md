# Shared database reference adapter

`kv_app.cc` contains a conditional LevelDB/RocksDB `EnvWrapper` and a deterministic
correctness/workload driver. `nvbridge.c` translates its filesystem operations
to the public NVFUSE runtime/API and links the caller-supplied archive.
`Makefile` is included by the two application-specific entry points. Generated
objects and executables go under the repository's ignored `build/integrations/`.

This is a source example, not an upstream database patch or production-complete
Env implementation. It supplies a fixed-size write buffer, process-local file
locks and a bounded set of operations. RocksDB mmap/random-RW/free-space and
other unimplemented methods return unsupported. Do not infer multiprocess DB
locking or general RocksDB feature support. Review the Env for your workload.

The workload uses 16-byte keys, 1KiB changing values, WAL enabled, sync=true,
compression disabled, 4MiB write buffers and a 16MiB database block cache.
It loads batches of 100, compacts outside the reported update phases, performs
warm random reads, then single-key and batch100 updates. Every key/value revision
is checked before close and again in a separate `verify` process. This workload
can fit in caches and does not represent a full-device database data set.

Arguments are `PATH ROWS SINGLE_UPDATES run|verify` plus `JSON BDF FORMAT` for
the native executable. Counts must be positive multiples of 100. Set
`KV_BATCH_ITERATIONS` to a multiple of 100 for the batch100 phase and keep it
identical for run and verify. Native FORMAT=0 reopens; FORMAT=1 is destructive
provisioning and is not part of the ordinary example commands.

Keep DB source/library/build options and adapter settings identical across core
comparisons. `NVFUSE_API_CALLER_EXEC=1` selects serialized caller execution for
metadata callbacks; the default uses owner dispatch. This adapter option must
also remain fixed. `KVBENCH`, `KVVERIFY` and optional core counter lines go to
stdout; save logs under `build/`, not in the source directory.
