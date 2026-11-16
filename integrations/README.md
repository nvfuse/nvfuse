# NVFUSE application integrations

Application adapters, build examples and reusable checks live here. The filesystem
and runtime remain in `../src/` and `../include/`; `make -f Makefile.spdk` builds
the core without any application dependency. Build adapters separately against
the same core headers/archive and SPDK build.

## Choose an integration

| Application | Entry point | Included source and scope |
| --- | --- | --- |
| fio | [Guide](fio/README.md) | API/SPDK engine, CRC/reopen checks and XFS comparison profiles |
| LevelDB | [Guide](leveldb/README.md) | Version-pinned reference Env, workload and independent reopen verification |
| RocksDB | [Guide](rocksdb/README.md) | Version-pinned reference Env, workload and independent reopen verification |
| LMCache | [Guide](lmcache/README.md) | Native connector, FIFO dispatcher, optional CRC acceleration and focused checks |
| ANN with DiskANN | [Guide](ann/README.md) | Version-pinned immutable graph reader replacement |
| Milvus | [Guide](milvus/README.md) | Knowhere graph reader routing and explicit shutdown |

[versions.json](versions.json) records dependency pins and API compatibility scope.
These are reference integrations, not a stable application-plugin SDK. Their
source layout has passed Linux x86_64 builds and focused correctness/reopen
checks for fio, LevelDB, RocksDB, the LMCache CPU bridge and diskannpy at the
pinned versions. Milvus native RPC/reopen remains an open release gate; see its
guide. Generated results are not shipped as proof that another host or
application version works.

## Build and use

Start with the [core user guide](../docs/user-guide.md) for dependencies, an image
quick start and exclusive device ownership. Primary API/SPDK requires Linux
x86_64 and SPDK v26.09. Supply each application's own pinned source/build;
third-party source trees, wheels and binaries are not vendored here.

```sh
make -f Makefile.spdk SPDK_ROOT_DIR="$SPDK_SOURCE"
make -C integrations help
```

Then follow the selected application guide. No default target provisions a
controller, downloads an application or runs a workload. Normal readers reopen
an existing filesystem. Formatting is a separate intentional provisioning step;
read each command before using it on a disposable test volume.

The core [external adapter contract](../docs/api-spdk-runtime.md#external-storage-adapters)
covers linking, request/DMA lifetime, shutdown and one-owner-per-volume rules.
Application API, scheduling, checksums and route choices belong to the adapter.
A reader replacement does not move every application storage operation to NVFUSE.

## Source and output policy

Commit reusable adapter source, build files, configuration examples, compatible
version references and usage/check instructions. Keep measurements, per-host
scripts, trace/stat snapshots, images, imported indexes, dependency trees and
binaries under the ignored repository `build/` directory or outside the checkout.
`integrations/.gitignore` also excludes common generated integration outputs.
Do not add result archives or full application images to a source PR.

For performance experiments, keep the application, adapter and its CRC/wait/queue
settings unchanged when comparing core revisions. Record total CPU budgets,
NUMA placement, caches, durability and recall with the results. Adapter tuning
and filesystem changes require separate comparisons.

Project adapter sources follow the root [COPYING](../COPYING) and
[NOTICE](../NOTICE); evaluate the combined application's distribution terms.

## Release checks

Run the [core release checks](../docs/releasing.md#verify-the-release-contents)
from an actual commit archive, then each application's focused checks using
sources from that same archive. Third-party applications and dependency builds
are supplied separately at the pinned versions; do not reuse a previous NVFUSE
archive, adapter binary or untracked application fixture. Retain command lines,
versions, hashes and failures under ignored `build/test-results/`.

| Integration | Required evidence |
| --- | --- |
| fio | CRC writes, separate-process CRC reads, supported block sizes and four-job mixed I/O |
| LevelDB / RocksDB | Native and POSIX build, WAL/sync updates and independent full-payload/per-key-revision verification |
| LMCache | Capacity/reclamation, FIFO overwrite order, close/drain, cold namespace and actual pinned LMCache CPU tensor bridge |
| ANN | New index, exact-ground-truth recall, full-byte graph import, positive native reads and clean shutdown/reopen |
| Milvus | Actual insert/flush/DISKANN/RPC query, native graph evidence, explicit runtime shutdown and new-server reopen |

The ANN and Milvus guides contain small fixture generators and verification
commands. Their data, traces and output JSON are generated files and remain
outside source commits. A mock, symbol check or build pass alone does not qualify
an application's end-to-end operation. Track application-server shutdown
failures separately from a successful NVFUSE unmount; both matter for delivery.
