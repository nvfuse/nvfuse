# Standalone filesystem development roadmap

Scope approved on 2026-11-12: implement roadmap stages 1 through 5. Physical
NVMe qualification and actual power-loss testing (stage 6) remain deferred.
Existing PR: #5, branch `local-dev-backend`. This checklist records implementation
status; design proposals are not counted as implemented or validated.

## 1. PR stabilization and continuous validation

- [x] Resolve the baseline macOS/Rocky Linux PR CI results (run 43211689897).
- [ ] Confirm both CI jobs for the new roadmap commit after push.
- [x] Replace the obsolete pending-test inventory with the executed matrix.
- [x] Schedule independent soak/chaos runs and preserve artifacts.
- [x] Provide an explicit periodic QEMU job with documented runner requirements.

## 2. Errors and large-volume resource admission

- [x] Replace volume-sized journal staging indexes with sparse structures.
- [x] Preflight sparse indexes and staging workspace at API boundaries.
- [ ] Prove strict operation credits and eliminate in-lock allocation in all legacy paths.
- [x] Return recoverable errors for journal allocation and direct-payload I/O failures.
- [ ] Convert remaining legacy cache/B+tree/partial-mount fail-fast paths to errno.
- [x] Exercise allocation/read/write/flush failures and subsequent handle reuse.

## 3. File semantics and persistent cleanup

- [x] Persistent orphan tracking for unlink/replace of open files.
- [x] Resumable large unlink/truncate cleanup and replay-safe block reuse.
- [x] Sparse extension and truncate growth with zero-fill and rollback checks.
- [x] Xattr and fallocate support with explicit error/durability contracts.

## 4. Performance and concurrency

- [x] Reproducible before/after latency, throughput and write-amplification data.
- [x] Group commit and asynchronous checkpointing with durable ordering tests.
- [x] Defined multi-thread/queue ownership, isolation and close semantics.
- [x] Asynchronous submission futures and filesystem-cache-bypass I/O; race/failure tests.
- [ ] Physical parallel SPDK queues and historical AIO prepare helpers (require a new ownership design).

## 5. User transaction evolution

- [x] Separate expected lookup misses from mutation/storage failures.
- [x] Larger bounded DATA redo groups without premature publication; evaluate COW.
- [ ] COW data mode and arbitrary-size streaming groups (require persistent allocation lifetimes).
- [x] Durable idempotency keys and outcome queries after lost commit responses.

Each format or concurrency change must document compatibility, recovery and
resource bounds before implementation. Every completed item needs executable
regressions and recorded evidence; baseline tests remain enabled. External
runner availability and physical-device qualification are distinct from code
completion. Do not infer completion of unchecked items from this document.

## Evidence and interpretation

See [roadmap-validation.md](roadmap-validation.md) for executed tests, artifact
identifiers and measured performance. Checked items describe the bounded
implementation, not full production qualification. Weekly workflows are defined;
activation requires merge and the dedicated QEMU runner remains unprovisioned.
The unfinished legacy unwind, physical SPDK parallelism and COW work must not be
inferred from passing tests of the supported paths. Stage 6 remains deferred.
