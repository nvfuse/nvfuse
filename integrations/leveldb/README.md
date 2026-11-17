# LevelDB integration

Pinned upstream version: **1.23**. This reference Env/workload is shared
with the other database under [common/db](../common/db/README.md). It is not an
upstream patch or installed database plugin. Read the shared scope and unsupported
operations before embedding it in another application.

## Build

Run from the repository root after building the matching NVFUSE core archive.
Build LevelDB 1.23 with its own CMake build, disabling upstream tests and
benchmarks if desired. Both its public include directory and source tree are
required because the reference driver disables mmap through the pinned internal
Env test helper. Supply that build's `libleveldb.a`.

```sh
make -C integrations/leveldb nvfuse \
  DB_SOURCE_DIR="$DB_SOURCE" DB_LIBRARY="$DB_STATIC_LIBRARY" \
  SPDK_ROOT_DIR="$SPDK_SOURCE"
make -C integrations/leveldb posix \
  DB_SOURCE_DIR="$DB_SOURCE" DB_LIBRARY="$DB_STATIC_LIBRARY"
```

Use the same `DB_EXTRA_LIBS` on both commands if the library requires them.
`NVFUSE_ROOT_DIR` and `NVFUSE_ARCHIVE` can select another compatible core checkout;
by default the current repository supplies both. No source patch is applied to
the upstream database. The Env implementation is in `common/db/kv_app.cc`.

## Run and verify

First prepare a disposable NVFUSE volume using the core user guide. Export the
[native runtime environment](../README.md#runtime-environment-for-native-examples).
The native path below is an
NVFUSE path; the POSIX path is an absent child directory on the chosen XFS mount.
Both runs create/write the specified database and keep it for verification.

```sh
export KV_BATCH_ITERATIONS=1000
build/integrations/leveldb/native/kv_app /leveldb-demo 1000 1000 run "$NVFUSE_SPDK_JSON" "$NVFUSE_SPDK_BDF" 0
build/integrations/leveldb/native/kv_app /leveldb-demo 1000 1000 verify "$NVFUSE_SPDK_JSON" "$NVFUSE_SPDK_BDF" 0
build/integrations/leveldb/posix/kv_app "$XFS_NEW_DB" 1000 1000 run
build/integrations/leveldb/posix/kv_app "$XFS_NEW_DB" 1000 1000 verify
```

Require `KVVERIFY ... full_payload=ok per_key_revision=ok` from both processes.
A new `run` requires a new database path; retain failed databases and logs.
These small counts are correctness examples. Choose and record an appropriate
working set, repetition count, CPU budget, cache and sync policy for performance
measurements. Build and runtime qualification applies to the pinned dependency
version; repeat these checks when changing the database or toolchain.
