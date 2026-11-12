# Validation status and remaining work

This file replaces the obsolete pre-journal checklist. The filesystem now replays
committed metadata from the original image after abrupt termination. Rejecting
an unclean mount is not the recovery success criterion.

## Executed coverage

See [resilience-tests.md](resilience-tests.md) for recorded artifact identifiers,
commands and limitations, and [metadata-boundary-tests.md](metadata-boundary-tests.md)
for the exhaustive boundary matrix.

| Area | Executable coverage |
| --- | --- |
| Resource exhaustion | Data and inode exhaustion, rollback, free counters, delete/reuse, independent raw checker |
| Address boundaries | Direct/single/double/triple indirect transitions, full uint32 inode space, capacity end and teardown |
| Persistence | Volatile/durable write model, flush errors, replay interruption, original-image reopening |
| Namespace accessibility | Cold parent checks, complete recursive enumeration, dot entries, full file reads and EOF |
| Large directories | CRC collisions, B+tree splits/deletes, name/depth boundaries, repeated reuse |
| Process failure | Persistent independent model across random process crashes |
| QEMU NVMe | Read/write errors, delays, timeout, removal, nine VM kill/recovery cases |
| Thread contract | Owner thread accepted; foreign threads rejected; exclusive process lock |
| Allocation | Journal allocation failures, retry/replay and handle reuse |

Passing owner-thread isolation tests does not establish concurrent filesystem I/O.
Prior stress and QEMU evidence applies to the revision recorded with each artifact;
changes require reruns of the affected suites.

## Continuous validation

`local-tests.yml` runs bounded PR tests on macOS and Rocky Linux 9.
`periodic-validation.yml` adds weekly soak and process-crash campaigns, plus an
opt-in dedicated ARM64 QEMU runner. Scheduled workflows become active on the
repository default branch after merge. The QEMU job requires repository variables
`NVFUSE_QEMU_ENABLED=true` and `NVFUSE_QEMU_VM_DIR`, a provisioned VM, QEMU, SSH,
and a runner labelled `nvfuse-qemu-arm64`. It is not automatically provisioned by
hosted CI. The guest verifies the dedicated NVMe serial `NVFUSEDEV0001`, device
mount state, PCI identity and IOMMU configuration before destructive testing.

## Remaining implementation and qualification

[development-roadmap.md](development-roadmap.md) is the current implementation
checklist; [roadmap-validation.md](roadmap-validation.md) records the latest
executed evidence. Persistent cleanup, sparse growth, inline string xattrs,
fallocate, bounded queued submissions/cache bypass, larger redo groups and
retained TX outcome keys are implemented. Full legacy error unwind, physical
parallel SPDK execution and COW remain outside the qualified implementation.
Physical NVMe and actual power interruption are explicitly deferred by the user.
