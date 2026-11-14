# NVFUSE documentation

**API + SPDK is the primary performance path. FUSE is the compatibility path.**
Start with the user guide; use the design documents when integrating or changing
the implementation. These documents describe the current checkout and its
explicit limits. Run-specific test reports belong under `build/test-results/`.

## Use and validate NVFUSE

| Document | Read it for |
| --- | --- |
| [Project README](../README.md) | Overview, six configurations, and a minimal image API quick start |
| [Release notes](../CHANGELOG.md) | Prerelease scope, format compatibility and known limits |
| [Source release preparation](releasing.md) | Distribution scope, clean-source checks and publication steps |
| [User guide](user-guide.md) | Dependencies, builds, C API example, device preparation, format/reopen, Linux mounts, monitoring and troubleshooting |
| [Validation guide](testing-guide.md) | Which test exercises which path; local/FUSE/SPDK/fio/metadata/fault coverage and result acceptance |
| [Interface/backend matrix](backend-matrix.md) | Build boundaries, standalone backend selection and exclusive ownership |
| [Primary API/SPDK runtime](api-spdk-runtime.md) | Shared core library, application integration and ownership contracts |
| [Primary fio integration](spdk-fio.md) | Modern asynchronous engine, environment variables, supported I/O and comparisons |
| [Live API/SPDK monitoring](spdk-live-monitoring.md) | iostat-style counters, multiple processes/devices, and metric limits |
| [Linux FUSE integration](linux-fuse-integration.md) | Mounted semantics, ownership, xattrs and POSIX compatibility limits |
| [Remaining work](../TODO) | Follow-up implementation and stability qualification tasks |

## Understand the core designs

| Document | Scope |
| --- | --- |
| [Metadata journaling](metadata-journaling-design.md) | Shared disk/journal layout, redo ordering, recovery and filesystem comparison |
| [Primary API/SPDK journaling](spdk-primary-journaling.md) | Serialized owner, asynchronous data fencing, primary sync policy and runtime options |
| [Experimental compact journal v3](journal-compact-log-v3-design.md) | Explicit format selection, append records, committed overlay and epoch reclamation |
| [User transactions](user-transactions-design.md) | Optional bounded standalone metadata/DATA transaction API; separate from primary automatic journaling |
| [Checkpoint scheduling](checkpoint-concurrency-design.md) | Standalone checkpoint/submission ownership and concurrency contracts |
| [Persistent cleanup](persistent-cleanup-design.md) | Standalone orphan/truncate reclamation and format compatibility |
| [Large transactions](large-transactions-design.md) | Journal geometry, admission limits and COW evaluation; not unlimited transactions |
| [Transaction outcomes](transaction-outcomes-design.md) | Durable standalone application retry/outcome records |

Primary journaling is enabled by default; new formats default to v2. V3 is
experimental and explicitly selected at format time. Standalone optional
transactions/checkpointing and the primary SPDK runtime have separate feature
contracts. Clean remount, process-crash recovery and physical power-loss
qualification are different evidence levels; consult the validation guide.
