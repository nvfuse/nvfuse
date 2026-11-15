# API/SPDK XFS parity investigation, 2026-11-14

## Latest fsync follow-up, 2026-11-15

Status: **focused LevelDB short-batch target achieved and integrated** on
`perf/xfs-parity`, based on `98848f3`. The wider all-workload XFS parity goal
remains open. This is the user-requested commit checkpoint; no push was made.
The original six-hour miss below is preserved; the follow-up completed within
the user's twelve-hour limit.

Same-device API/SPDK versus native XFS, journal v3/WAL ON, every transaction
sync=true, unchanged application/compaction policy and fixed preceding work.
The prior optimized baseline and new defaults use the same final binaries.
New settings are explicitly unset for the default arm.

| LevelDB batch100 /100k, ops/s | XFS | Previous optimized baseline | Integrated defaults | Default/baseline | Default/XFS |
|---|---:|---:|---:|---:|---:|
|XFS first, median3|159235.70|143719.84|171847.04|+19.57%|+7.92%|
|NVFUSE first, median5|160136.13|144040.15|177395.23|+23.16%|+10.78%|

Mean advantages are+9.05% and+9.38%, respectively. In each comparison every
new-default sample exceeds every fresh XFS sample. Final reverse raw values,
broader application results and all unsuccessful experiments remain below.
LevelDB long1M batch is+11.71% versus XFS in the final broad comparison.
RocksDB short/long remain-1.24%/-1.20% versus XFS; its single-update throughput
regresses2.83–3.16% versus the previous NVFUSE baseline. No all-application
or new headline fio performance victory is claimed.

The main causal improvement is reduced owner serialization during storage
waits. File-scoped fsync alone was insufficient. It now collects target data,
inode/indirect metadata and target allocation dependencies from committed
shared-metadata images. A single bounded cache executor can progress while
immutable commit or target-excluded data completions are awaited. DMA target
aliases, lifecycle operations, generic callbacks and unsupported dependencies
retain exclusion/full fallback. Ordered data -> durable log -> fsync ACK is
unchanged. B+tree, inode/disk layout, block-group ownership, cache policy and
control-plane design are retained.

`NVFUSE_JOURNAL_FILE_SYNC`, `NVFUSE_API_COMMIT_CACHE` and
`NVFUSE_API_DATA_CACHE` default ON subject to their standalone/v3 eligibility.
Set all three to0 for the prior behavior. Existing volume versions and the
default format version do not change. See [design](file-scoped-fsync-design.md).

Qualification: final SPDK build18 commands; exact122 successful commands
(110 before a preserved harness launch error,12 in corrected resume); full/default
73 outer commands;23 retained images independently replayed/checked;42 broad
and18 reverse app trials including smokes and all fresh-process reopens;
CRC32MiB write/read plus six fio functional profiles in each setting; all PASS.
Two harness launch/preflight mistakes are retained and explained below. No
product/binary changes were needed for those corrections. Server independent
checkers ran without unavailable GCC sanitizer runtimes; native journal,
transaction and boundary tests passed. This is software recovery/persistence
qualification, not a physical power-cut test. The existing baseline
truncate-gap content defect described below remains outside this append-focused
change; its affected behavior is not reported as fixed or qualified here.

Scope: warm100k-row/1KiB-value DB, NVFUSE512MiB cache versus unrestricted XFS
page cache, four application CPUs each plus two additional NVFUSE runtime cores.
The two filesystem orders use the same selected SSD sequentially. An unrelated,
previously failed dd-device soak remains failed and unchanged; the host is not
idle. This is not a multi-terabyte application evaluation. Prior equal-volume
fio acceptance criteria and remaining sequential-profile gaps are unchanged.

Final live audit at2026-11-15 02:26:39 UTC passed: approved controller4d/serial
BIDCT0002I040CJ1H is back on kernel nvme as /dev/nvme23n1, unmounted/unused;
node0 hugepages restored to1024; no media errors or unrelated driver changes;
all measured source/binary hashes and the failed dd-soak state preserved.

Evidence: `build/test-results/fsync-final-20261115/` contains source manifest,
raw/analysis JSON for both orders, original and corrected runners, failed logs,
retained-image checker reports, `qualification.json` and `final-live-audit.json`.
The exact built code matches193 local source/test/build/script hashes. Only
report documentation changed after the build.

### Commit checkpoint: consolidated application results

This checkpoint includes the adopted buffered mapping/full-page-copy/retry
optimizations, file-scoped fsync, bounded commit/data-wait cache execution,
regression tests and evaluation tooling. Commit creation was requested after
qualification; the dated experimental notes below retain their original status.
No push or release is included.

The table uses the final broad comparison, three full trials per arm. Values
are median operations/s. Short/long labels identify the final batch phase size;
the load, warm-read and single-update phases precede that phase.

| Application / batch updates | Phase | XFS | Prior NVFUSE | Default NVFUSE | Default/prior | Default/XFS |
|---|---|---:|---:|---:|---:|---:|
| LevelDB / 100k | Load batch100 | 248,241.78 | 265,099.49 | 266,313.45 | +0.46% | +7.28% |
| LevelDB / 100k | Warm read | 173,450.66 | 177,274.66 | 177,536.21 | +0.15% | +2.36% |
| LevelDB / 100k | Single sync update | 32,357.18 | 35,192.10 | 39,223.72 | +11.46% | +21.22% |
| LevelDB / 100k | Sync batch100 | 159,235.70 | 143,719.84 | 171,847.04 | +19.57% | +7.92% |
| LevelDB / 1M | Load batch100 | 249,281.85 | 266,403.63 | 266,544.77 | +0.05% | +6.93% |
| LevelDB / 1M | Warm read | 174,369.59 | 176,875.86 | 176,937.75 | +0.03% | +1.47% |
| LevelDB / 1M | Single sync update | 32,392.33 | 35,232.16 | 39,455.06 | +11.99% | +21.80% |
| LevelDB / 1M | Sync batch100 | 123,979.49 | 132,163.50 | 138,496.13 | +4.79% | +11.71% |
| RocksDB / 100k | Load batch100 | 216,992.95 | 221,073.12 | 219,806.77 | -0.57% | +1.30% |
| RocksDB / 100k | Warm read | 176,639.92 | 179,962.05 | 180,419.46 | +0.25% | +2.14% |
| RocksDB / 100k | Single sync update | 30,750.37 | 39,647.31 | 38,524.98 | -2.83% | +25.28% |
| RocksDB / 100k | Sync batch100 | 188,447.37 | 186,730.29 | 186,115.46 | -0.33% | -1.24% |
| RocksDB / 1M | Load batch100 | 217,662.53 | 221,231.42 | 219,886.86 | -0.61% | +1.02% |
| RocksDB / 1M | Warm read | 178,283.73 | 180,008.26 | 179,815.39 | -0.11% | +0.86% |
| RocksDB / 1M | Single sync update | 30,868.18 | 39,590.05 | 38,337.45 | -3.16% | +24.20% |
| RocksDB / 1M | Sync batch100 | 186,718.02 | 179,477.51 | 184,484.49 | +2.79% | -1.20% |

The five-trial reverse confirmation above applies only to LevelDB short.
RocksDB short/long batch and its single-update regressions remain open.
The initial historical LevelDB deficit (-12.75%) and the final OFF arms come
from different runs; use each matched OFF/default comparison for improvement.

Tracked [result samples and provenance](performance-results/fsync-20261115.json)
preserve every final application phase sample, qualification counts, source
archive identity and raw-result hashes. Large raw logs/images stay in the local
ignored evidence directory; they are not embedded in this commit.

Fio throughput was last compared **before** this fsync change: equal-volume
4KiB random read +14.23%, random write +31.89%, mixed +7.60%; 256KiB sequential
write +3.87%, sequential read -1.10%, sequential mixed -1.23% versus XFS.
The separate time-based write-history/read workload was -14.35%. These are
historical results, not final-code throughput claims. This final code passed
32MiB CRC write/read and six fio functional profiles for both settings.

Remaining work: RocksDB batch parity and single-update regression; a complete
final-code fio throughput refresh with equal preceding write volume; physical
power-cut qualification; and the recorded baseline truncate-gap defect.

## Historical status before the fsync follow-up

Status: **goal unmet**. Work continues on `perf/xfs-parity`, based on `98848f3`.
The focused six-hour LevelDB investigation ended **without meeting the target**;
its deadline was missed. No candidate from that investigation was adopted.
Final results, the qualified cause finding and cleanup are recorded at the end
of this report. The complete matched table below remains the prior checkpoint.
User-approved follow-up physical comparison `commit-cache-20261114` also failed
the short target: XFS164667.33, current-behavior OFF143831.75, candidate143494.15
ops/s (-12.86% vs XFS); candidate remains private. Existing-counter diagnosis
and same-binary commit-cache-enabled10/50us retry control follow at the end.
The latest uninstrumented50us matched comparison has primary equal-volume4KiB
read+14.23%, random write+31.89% and random mixed+7.60%. Remaining material
application gaps include LevelDB100k batch-12.75% and RocksDB1M batch-3.13%.
Post-timer analysis locates RocksDB excess in the slowest1%; journal-span and
CF-stat diagnostics instead point toward L0 compaction/write delays. LevelDB's
short-batch excess lies mainly in the other99%. The private LRU-head candidate
failed its performance screen and was not imported.
Current exact-binary profiles independently confirm cached-read mutex retry
cost on RocksDB compaction; the isolated retry-probe cadence candidate failed its performance screen
and was not imported. Aligned full-page CPU-copy calibration found no benefit; later partial/misaligned
calibration supports a conditional read-copy candidate. Its first experiment
has a diagnostic counter/timer cacheline confound. The corrected64byte counter-isolation main control passed, but its unchanged-binary reversed confirmation failed the short-batch loss screen. The candidate is rejected and absent from production.

The last implementation work before release preparation was `6da7672`
(serialized buffered API I/O and writeback); `3bf166e` moved the shared runtime,
and `98848f3` prepared the source prerelease. Release preparation did not finish
application performance optimization.

## Reproduction and evidence

Physical host: `LNSTRTYO3SSD1507`. Disposable benchmark controller:
`0000:4d:00.0`, serial `BIDCT0002I040CJ1H`, capacity `7681501126656` bytes.
Each physical stage rechecks identity, mounts/holders/users/swap, SMART and the
independent soak on `0000:dd:00.0`. It restores the selected controller driver,
override and the hugepage increment. Unrelated device changes are recorded
separately. No global device-binding script is used.

Evidence is under `build/test-results/xfs-parity-20261114/remote-evidence/`;
remote originals are under `/home1/irteamsu/nvfuse-dev/run/`. Every stage retains
commands, logs, JSON results and source/archive/linkage proofs. Large raw perf
recordings remain on the server. These paths contain experimental evidence and
are not release inputs.

The latest completed matched run is `parity-retry-budget-20261114`, including
default-ON full-page-only initialization and bounded50us caller-cache retry (10us backout): journal v3 ON,
512 MiB NVFUSE cache, I/O CPU6, FS CPU8, application CPUs10/12/14/16,
SPDK26.09/fio3.41, three trials per configuration. The six fio profiles use four
fully written20GiB files, QD32 per job,10s measurement plus2s ramp, direct I/O
and final fsync. DBs use WAL and `sync=true` for every transaction,100k initial
rows,1KiB values,100k single updates, and separate **100k and1M batch updates** of100 values.
The older `xfs-parity-final-20261114` used the long1M scenario only.

Limits: NVFUSE uses two additional runtime cores; filesystem cache caps differ;
an independent soak and unrelated host activity continue; XFS ran before
NVFUSE. Process-crash/reopen tests do not qualify physical power loss. Fio final
fsync and DB per-transaction sync are distinct durability workloads. No journal
barrier or successful-fsync guarantee was relaxed for these changes.

## Prior adopted XFS comparison before the fsync follow-up

User-selected acceptance criterion: **4KiB random read uses the same explicit preceding write volume as its primary metric** (80GiB sequential rewrite plus160GiB fixed-seed random overwrite per backend). The original time-based write-then-read profile is retained as a separate workload result. This controls the explicit preceding pass, not the complete SSD history. Current primary read result is+14.23% versus XFS; the goal remains unmet because sequential read/mixed and DB batch gaps remain.

Three-trial medians from `parity-retry-budget-20261114`, exact adopted source with
buffered mapping, full-page initialization and bounded50us caller-cache retry enabled (10us backout supported). Build, all v2/v3 checks,
full80GiB CRC32C write/read verification and device cleanup passed.

| fio profile | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|256KiB sequential write, MiB/s|5,847.78|6,073.82|+3.87%|
|256KiB sequential read, MiB/s|6,816.67|6,741.80|-1.10%|
|256KiB75% read mixed, MiB/s|8,909.92|8,800.70|-1.23%|
|4KiB random write, IOPS|1,095,904|1,445,388|+31.89%|
|4KiB random read, IOPS (time-based write history; secondary)|1,381,295|1,183,046|-14.35%|
|4KiB75% read mixed, total MiB/s|4,943.93|5,319.65|+7.60%|

| DB phase, operations/s | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|LevelDB load|252,532|260,628|+3.21%|
|LevelDB warm read|178,480|179,040|+0.31%|
|LevelDB single sync update|32,462|35,128|+8.21%|
|LevelDB batch100,100k updates|163,405|142,568|-12.75%|
|LevelDB batch100,1M updates|125,205|121,971|-2.58%|
|RocksDB load|217,395|220,877|+1.60%|
|RocksDB warm read|180,112|180,085|-0.02%|
|RocksDB single sync update|31,152|39,670|+27.34%|
|RocksDB batch100,100k updates|186,958|188,613|+0.89%|
|RocksDB batch100,1M updates|186,473|180,628|-3.13%|

The extra equal-volume read control follows an80GiB sequential rewrite and
exactly160GiB random overwrites in each filesystem, with a fixed random seed.
This fixes that preceding pass, not the entire accumulated device history.

| Primary random-read criterion,4KiB IOPS | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|same explicit preceding write volume|1,488,809|1,700,643|+14.23%|

Warm-read throughput includes full value reconstruction/comparison in the
workload; it is not a filesystem-only read-bandwidth measurement. None of the
later isolated candidate gains can be added to these deltas as a new measured
XFS result.

<details>
<summary>Historical10us checkpoint: parity-caller-cache-20261114</summary>

Three-trial medians from `parity-caller-cache-20261114`, exact adopted source with
buffered mapping, full-page initialization and bounded caller-cache retry enabled. Build, all v2/v3 checks,
full80GiB CRC32C write/read verification and device cleanup passed.

| fio profile | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|256KiB sequential write, MiB/s|5,858.14|6,076.91|+3.73%|
|256KiB sequential read, MiB/s|6,816.17|6,741.85|-1.09%|
|256KiB75% read mixed, MiB/s|8,924.26|8,807.50|-1.31%|
|4KiB random write, IOPS|1,096,657|1,446,408|+31.89%|
|4KiB random read, IOPS (time-based write history; secondary)|1,388,439|1,143,387|-17.65%|
|4KiB75% read mixed, total MiB/s|4,985.97|5,218.79|+4.67%|

| DB phase, operations/s | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|LevelDB load|252,443|262,021|+3.79%|
|LevelDB warm read|177,700|178,473|+0.43%|
|LevelDB single sync update|32,540|35,623|+9.47%|
|LevelDB batch100,100k updates|163,509|151,147|-7.56%|
|LevelDB batch100,1M updates|124,793|123,017|-1.42%|
|RocksDB load|217,840|220,911|+1.41%|
|RocksDB warm read|178,398|180,243|+1.03%|
|RocksDB single sync update|30,789|39,497|+28.28%|
|RocksDB batch100,100k updates|188,095|187,709|-0.21%|
|RocksDB batch100,1M updates|187,183|178,893|-4.43%|

The extra equal-volume read control follows an80GiB sequential rewrite and
exactly160GiB random overwrites in each filesystem, with a fixed random seed.
This fixes that preceding pass, not the entire accumulated device history.

| Primary random-read criterion,4KiB IOPS | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|same explicit preceding write volume|1,491,628|1,701,788|+14.09%|


</details>

<details>
<summary>Historical pre-caller-retry checkpoint: parity-combined-20261114</summary>

Three-trial medians from `parity-combined-20261114`, exact adopted source with
buffered mapping and full-page-only initialization enabled. Build, all v2/v3 checks,
full80GiB CRC32C write/read verification and device cleanup passed.

| fio profile | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|256KiB sequential write, MiB/s|5,853.93|6,072.25|+3.73%|
|256KiB sequential read, MiB/s|6,816.64|6,742.35|-1.09%|
|256KiB75% read mixed, MiB/s|8,921.49|8,801.38|-1.35%|
|4KiB random write, IOPS|1,091,527|1,446,118|+32.49%|
|4KiB random read, IOPS|1,379,245|1,191,384|-13.62%|
|4KiB75% read mixed, total MiB/s|4,960.19|5,395.47|+8.78%|

| DB phase, operations/s | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|LevelDB load|250,908|261,011|+4.03%|
|LevelDB warm read|175,535|179,277|+2.13%|
|LevelDB single sync update|32,415|35,306|+8.92%|
|LevelDB batch100,100k updates|161,464|151,478|-6.18%|
|LevelDB batch100,1M updates|123,779|122,466|-1.06%|
|RocksDB load|217,145|220,923|+1.74%|
|RocksDB warm read|177,331|180,656|+1.87%|
|RocksDB single sync update|30,808|38,854|+26.12%|
|RocksDB batch100,100k updates|187,786|188,392|+0.32%|
|RocksDB batch100,1M updates|186,299|177,312|-4.82%|

The extra equal-volume read control follows an80GiB sequential rewrite and
exactly160GiB random overwrites in each filesystem, with a fixed random seed.
This fixes that preceding pass, not the entire accumulated device history.

| Extra read control,4KiB IOPS | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|same explicit preceding write volume|1,488,604|1,700,191|+14.21%|


</details>

<details>
<summary>Historical pre-full-copy checkpoint: parity-refresh-20261114</summary>

Three-trial medians from `parity-refresh-20261114`, exact adopted source with
the bounded buffered write-mapping change enabled. Build, all v2/v3 checks,
full80GiB CRC32C write/read verification and device cleanup passed.

| fio profile | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|256KiB sequential write, MiB/s|5,625.59|6,043.79|+7.43%|
|256KiB sequential read, MiB/s|6,817.34|6,726.91|-1.33%|
|256KiB75% read mixed, MiB/s|8,928.38|8,805.82|-1.37%|
|4KiB random write, IOPS|1,084,987|1,442,989|+33.00%|
|4KiB random read, IOPS|1,388,115|1,194,621|-13.94%|
|4KiB75% read mixed, total MiB/s|4,934.62|5,432.15|+10.08%|

| DB phase, operations/s | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|LevelDB load|250,256|259,501|+3.69%|
|LevelDB warm read|177,144|177,879|+0.41%|
|LevelDB single sync update|32,362|35,085|+8.42%|
|LevelDB batch100,100k updates|161,490|151,457|-6.21%|
|LevelDB batch100,1M updates|124,797|121,143|-2.93%|
|RocksDB load|215,553|217,446|+0.88%|
|RocksDB warm read|176,133|179,554|+1.94%|
|RocksDB single sync update|30,704|38,716|+26.10%|
|RocksDB batch100,100k updates|186,023|180,347|-3.05%|
|RocksDB batch100,1M updates|177,038|175,571|-0.83%|

The extra equal-volume read control follows an80GiB sequential rewrite and
exactly160GiB random overwrites in each filesystem, with a fixed random seed.
This fixes that preceding pass, not the entire accumulated device history.

| Extra read control,4KiB IOPS | XFS | NVFUSE | NVFUSE change |
| --- | ---: | ---: | ---: |
|same explicit preceding write volume|1,477,271|1,697,579|+14.91%|


</details>

## Read scaling and conditioning

In the latest retry-budget run, fio's reported timed4KiB write bytes across the
three preceding write trials total125.520GiB for XFS versus165.293GiB for NVFUSE.
These reported intervals exclude the separate ramp and do not represent all
physical NAND writes. Equal wall-clock write trials therefore do not impose
equal preceding logical write volume. The fixed-volume read control addresses
that explicit pass only; it does not prove a particular device/FTL cause.


The earlier `xfs-parity-final-20261114` run found NVFUSE485449 IOPS versus XFS372162 at1job/QD32,
and1680469 versus373653 at1job/QD128. Each uses only one20GiB file. Four jobs
activate80GiB, so these numbers alone do not isolate thread count.

`postwrite-read-20261114` therefore compared the same total80GiB at totalQD128,
using the unchanged combined binary. Three-trial medians:

| preceding writes |1job x80GiB, QD128|4jobs x20GiB, QD32 each|
| --- | ---: | ---: |
|sequential overwrite|1,684,957|1,693,838|
|random overwrite|685,914|1,194,626|

Both configurations share one FS owner and one I/O reactor. The sequentially
conditioned result is consistent with a common throughput ceiling. The much
larger subsequent drop occurs without a source change, including in the
single-thread control. Write history, device state and physical file placement
must therefore be separated from CPU scaling. These are different files/LBA
ranges, measured serially; the specific cause is still unproven. They are not
new XFS comparisons. Mapper hit rates above99.8% in the previous four-job
post-write run rule out a simple explanation based only on cold mapper misses.

The exact historical old plugin also reaches about1.57M in current controls;
the older1.048M result is not established as a compiler/source-move regression.
Shared versus split worker affinity was flat around1.577M. Global perf cycle
percentages combine fio wait loops and both reactors. Per-comm normalization
found FS self costs of12.21% in the mapper,4.51% in hash lookup and4.31% in the
serialized-page helper; the I/O core spent11.04% in SQ request retrieval. The
sample includes startup/teardown, so these are diagnostic function shares and
not direct saturation estimates.

## Adopted changes and independent measurements

- Separate producer/consumer queue cursors onto cache lines and avoid publishing
  unchanged empty-CQ cursors. Four-job read improved about1.6% in its own control;
  corrected DB linkage measurements were effectively flat.
- Resolve authoritative cached read mappings while holding the exclusive FS
  gate, with conservative fallbacks and no retained evictable pointers.
  Same-binary OFF/ON trials improved4jobs/QD32 about7.0%;1job/QD128 about1.8%.
  Backout: `NVFUSE_AIO_OWNER_MAPPING=0`.
- Reuse at most32 contiguous scalar block mappings within a buffered write and
  its held inode lock; copy full-page misses into eligible clean/unused entries
  without a transient REF-list trip. Same-binary ABBAAB long-batch trials in
  `write-extent-20261114` improved RocksDB171205 to174798 (+2.10%); LevelDB was
  effectively flat130594 to130786 (+0.15%). Backout:
  `NVFUSE_BUFFERED_EXTENT_MAPPING=0`. The latest matched XFS comparison above
  includes this change.

V2/v3 image tests, fragmented overwrite, partial blocks, truncate/regrow,
fsync-then-process-exit with two cold reopens, fault recovery, fio CRC, and real
DB payload/revision oracles passed for the write candidate. DB eviction coverage
uses16MiB filesystem cache against roughly100MiB of live values. The combined
queue/read-mapper run also checked actual four-file sizes/tails and executed the
modified six-profile Python runner. Portable completion/boundary checks pass.

Rejected earlier screens include64KiB speculative writeback start, vector32,
lock ownership hints, and an unreproduced wait-yield gain. Historical dirty-wait
percentages describe nested old diagnostic spans, not the present end-to-end
bottleneck share.

## Outstanding work

The payload-initialization experiment passed v2/v3 image, DB oracle/reopen,
small-cache and cleanup checks, but was rejected: LevelDB long batch fell
129918 to127688 (-1.72%) while RocksDB rose174722 to176748 (+1.16%) in its own
same-binary comparison. It is not imported. A separate SQ-consumption experiment
tested bounded32-request copies/publications. It passed the4M queue test,
v2/v3 images, physical CRC and cleanup, but was also rejected: four-job random
read1687354 to1677100 (-0.61%), one-job read unchanged, sequential read -0.32%.
The pre-measurement build/logging and counter-reset harness failures remain in
the evidence alongside the corrected final run. Neither candidate is imported.

`parity-refresh-20261114` passed, including all DB payload/revision oracles,
cold reopen, both batch sizes, the full80GiB CRC and actual inode/tail audits.
Its exact source archive is SHA256
`b287d397038f8b76d7f98fcb9a5036b3a9ea77457dd4e1a189692cd14d0016f5`.
Before full-copy adoption, all140 C/header/include files matched that archive;
source-manifest verification covered279 archived files before building.
The selected controller, hugepage increment and independent soak checks passed.

The standard-order random read remains13.94% below XFS, but after the equal
explicit write volume it is14.91% above XFS, with no binary change. This is a
stronger control for write-history sensitivity; the cause is not yet established.
It does not justify declaring every read regression solved or blaming the
single FS owner alone. The older normalized perf includes startup/teardown.
The first standard/control read trials each had exactly20,480 mapper fallbacks
(including startup), while fio mean completion latency changed106.79 to75.07us
and average FS submit batch1.71 to5.95. This is not isolated device latency.

`wb-compact-20261114` passed full recovery, DB oracle/reopen,16MiB-cache and
cleanup checks, but was rejected. Allocated I/O objects fell71%/75% for
LevelDB/RocksDB, yet same-binary ABBAAB long-batch medians changed
130493 to130278 (-0.16%) and175223 to175671 (+0.26%). Ranges overlap; there
is no convincing end-to-end gain. It is not imported.

`readstats-20261114` passed, with read-only four-file CRC validation on both
v2/v3 images, final physical inode/tail/EOF audit and device cleanup. It used
identical four20GiB files for one/four fio threads. Existing SPDK per-channel
statistics were queried only at shutdown, with no per-I/O instrumentation.
Bdev latency includes SPDK queue/polling and startup metadata; fio clat uses the
timed interval. Their difference is not an exact CPU-time attribution.

| NVFUSE read condition | trials | totalQD | IOPS | bdev mean us | fio clat mean us |
| --- | ---: | ---: | ---: | ---: | ---: |
|same four files,1thread|3|128|1,709,724|66.08|74.52|
|same four files,4threads|3|128|1,711,048|66.18|74.50|
|4threads,larger depth|1|256|1,717,387|139.38|148.75|
|4threads,largest depth|1|512|1,717,551|285.15|297.78|
|after3timed random-write trials|3|128|1,155,390|105.74|110.44|
|after full-range refreshed-buffer writes|3|128|1,712,506|66.43|74.44|

The common~1.71M ceiling exists with either frontend thread count. Increasing
QD did not materially increase throughput in the single-trial depth checks.
After timed random rewrites, the~33% throughput drop accompanied bdev mean
latency rising~66 to106us. Full-range80GiB sequential+160GiB random overwrites
with `refill_buffers=1` restored~1.71M and~66us. Thus the drop is strongly tied
to the I/O path's state after writing, even with refreshed buffer data. It is
not established as an FTL/NAND defect, thermal issue, or specific FS-core
saturation. The two runtime cores remain additional CPU resources; one fio
thread is not a one-core filesystem result. A general parallel-FS redesign
is not justified solely by this read deficit.

The initial one-thread/four-file run was rejected by the adapter's `nrfiles=1`
guard, before data I/O; `failed-nrfiles/` retains failure+cleanup. A diagnostic
opt-in read-only four-file adapter was image-CRC-tested before resuming; it is
not imported. The final marker assertion initially looked in process stdout,
but fio put the marker in its JSON-output prefix. `failed-marker-stream/`
retains that failure+cleanup. Post-validation checked all14 measured original
output files, actual four-file/size/depth options, bdev statistics and the
missing final read-only audit. No throughput trials were selectively rerun;
`post-validate.py` and `post-validate.log` record completion.

`full-copy-20261114` passed the full v2/v3 image suite, all main ABBAAB
measurements, small-cache integrity/reopen checks and cleanup. It skips zero-before-copy only for a
proven full-page mapped cache miss. Partial/append initialization is unchanged.
Long-batch medians are LevelDB130720 to130399 (-0.25%, overlapping ranges) and
RocksDB175246 to177174 (+1.10%, nonoverlapping ranges). The unchanged binary completed a reversed BAABBA confirmation after the
read/profiling stages; this candidate was adopted after that confirmation below.

Two isolated read candidates run strictly after preceding PASS+cleanup and use
the same adopted archive, with separate overlays and OFF/ON ABBAAB trials:

- `aio-credits-20261114`: retain conservative worst-case admission, then debit
  the actual published device-job count instead of the logical block count.
  A256KiB request usually needs1-2 device jobs instead of64 reserved entries.
- `compact-aio-20261114`: allocate one job only when the authoritative serialized
  read mapper proves the entire request contiguous; fragmented/cold/boundary
  probes retain worst-case allocation. This excludes the credits overlay.

Both run full v2/v3 image suites, wide-I/O physical CRC, actual inode/tail audits,
three trials per arm of256KiB sequential read, and unchanged4KiB read controls.
Neither read candidate is adopted or part of the matched XFS table above.

Remaining parity work includes standard-order random read variability, the
roughly1.3% sequential read/mix gaps, and short/long DB batches.
Disk layout, B+tree, block groups, cache replacement policy and control-plane
design remain unchanged; unused-file cleanup is outside this work.

`batch-profile-20261114` passed both DB oracles/reopens and cleanup. It uses the exact
`parity-refresh` checkpoint, with perf sampling enabled only after the single
update marker and disabled at the long-batch marker. Per-thread CPU-tick snapshots
bound the same interval. Profiled throughput is diagnostic and will not replace
unprofiled matched comparison results.

`aio-credits-20261114` passed v2/v3 images,256MiB wide-I/O physical CRC,
inode/tail audits, actual-use counters (lane mask1 ON versus15 OFF) and cleanup.
It is rejected: sequential read26512.95 to26483.06 IOPS (-0.11%),4KiB read
1702019.50 to1701679.03 (-0.02%). These do not show an end-to-end gain.
`compact-aio` passed the image suite, physical CRC, actual-use counters, inode
audits and cleanup. It is also rejected: sequential26475.76 to26494.75 (+0.07%),
4KiB1702098.79 to1702435.36 (+0.02%). No convincing gain.

The batch-only comm-filtered profile found LevelDB application threads spending
24.19% self samples in CRC,19.44% in value generation,5.91% in cached pread,
4.96% in append,4.30% in completion polling,3.76% in pthread trylock and2.39% in
hash lookup. `kv_app` merges LevelDB main/background threads, so this is not a
main-thread-only attribution. RocksDB `kv_app` self samples include value37.88%,
CRC17.73%, completion polling14.61%, and memmove7.10%. Percentages from different
comm filters cannot be compared as total process/CPU shares. Exact-TID analysis
of the saved data subsequently passed; the per-TID results below supersede
main-thread interpretations of the comm-filtered report.

`gate-ttas-20261114` completed after the full-copy confirmation. It tested a
read-mostly busy hint during the existing bounded gate spin, while retaining the
original pthread mutex as the sole exclusion and publication mechanism. It
clears the hint before unlocking to prevent a previous holder clearing a new
holder's hint. Stale hints can only delay a bounded retry; they never authorize
entry. The experiment uses the original adopted archive, excluding all later
candidate overlays, with full API concurrency/recovery and DB/cache16 checks.

The gate candidate's predefined adoption screen requires at least one long-batch
median gain above0.5%, with neither DB losing more than0.5%. Only candidates
passing that screen run OFF/ON ABBAAB sequential/4KiB read regression checks.
The screen is not proof of significance or final XFS parity; a fresh matched
comparison is still required before reporting a new combined XFS result.

`full-copy-confirm-20261114` passed all full-payload/revision oracles, fresh
reopens and cleanup using the unchanged original full-copy binary, reversed
BAABBA order, three trials per arm. LevelDB129612.66 to130548.96 (+0.72%) and
RocksDB174459.78 to176863.85 (+1.38%). The initial LevelDB -0.25% did not repeat;
the RocksDB benefit repeated. The full-page-only change is now adopted locally,
default ON only for journaled standalone primary. Backout:
`NVFUSE_BUFFERED_FULL_COPY=0`. Partial/append initialization remains unchanged.
The default change and combined source passed exact-source build, full v2/v3
revalidation and the complete matched XFS comparison above, including cleanup.

The phase-marker CPU tick snapshots approximately bracket7.786s (LevelDB) and
5.831s (RocksDB) under profiling. LevelDB main used4.10 CPU-s (~52.7%), its
background thread7.43s (~95.4%); RocksDB main5.08s (~87.1%), low4.94s (~84.7%),
high0.87s (~14.9%). I/O reactor CPU was near100% and FS reactor97.6%/82.1%,
including idle polling. These are CPU execution percentages, not useful-work
or saturation estimates. The main/background split motivates checking
compaction work and shared gate cost without changing sync semantics.

The fresh exact-source comparison completed as `parity-combined-20261114` after
the gate stage and saved-profile exact-TID analysis passed and released the
shared lock. It includes adopted full-copy, excludes TTAS/credits/compact,
and uses archive SHA256
`f3073ca5ca5d2c13fb79edf79a6d7e0330f0bec863513dd5a0818ba70af39f33`.
All140 current C/header/include files match this snapshot (278 manifest entries
plus the manifest itself). Remote manifest verification, build and default-on
v2/v3 validation and physical comparison passed, both with cleanup confirmed.
The latest table above is this complete run, with all50 fio invocations and
28 DB runs retained (including smoke trials), each with a fresh-process reopen.

Exact-TID post-analysis passed without new measurements. Self samples within
each named thread (not global CPU or wall-time shares):

| thread | approx CPU% | leading self sample costs |
| --- | ---: | --- |
|LevelDB main|52.7|value generation61.81%, completion polling13.67%|
|LevelDB background|95.4|DB CRC35.29%, cached pread8.62%, append7.23%, trylock5.49%, hash3.48%|
|RocksDB main|87.1|value37.88%, DB CRC17.73%, completion polling14.61%|
|RocksDB low background|84.7|mapped cache write13.80%, pread11.77%, XXH11.22%, trylock4.99%|

CRC and value generation belong to the application workload/library, not the
NVFUSE journal. This diagnostic uses the pre-full-copy checkpoint; full-copy's
benefit is supported by separate unprofiled comparisons, not inferred from
sampling percentages. The TTAS candidate passed all validation/cleanup but was
rejected: LevelDB129044.91 to128686.50 (-0.28%), RocksDB174608.86 to174673.98
(+0.04%). Its predefined DB screen failed, so fio performance screening was
explicitly skipped. It is not imported.


## Isolated clean-period drain experiment (rejected)

`clean-drain-20261114` completed after the combined comparison's PASS and cleanup. Its source is a separate default-OFF overlay on the combined
archive; the working implementation tree is unchanged. The owner currently
stops AIO submissions at every periodic deadline, even when the existing commit
predicate (`journal_dirty || nvfuse_get_dirty_count`) is false. Because
`journal_aio` includes reads, this empties a clean read queue every100ms.

The experiment checks that same predicate under the existing exclusive FS gate,
advances only a clean deadline and continues submitting. Dirty-period draining,
commit ordering and all explicit sync paths are unchanged. It records only
per-period clean skips/completed drains/dirty commits. Full v2/v3 concurrency,
crash/recovery, CRC and runner checks precede physical ABBAAB controls. Each arm
has the same explicit80GiB sequential and160GiB random conditioning pass before
256KiB read,4KiB read and256KiB75%-read mixed trials. The full experiment passed but did not show a useful throughput gain and was
rejected. It is not imported.


`writer-owner-20261114` started strictly after clean-drain PASS+cleanup. It
uses a separate default-OFF overlay on the combined full-copy snapshot, excluding
the clean-drain candidate. Only typed `nvfio_pwrite` can establish a transient
exclusive-cache proof while holding `fs_gate`, without AIO clients, on the
journaled standalone primary. Three existing bounded write helpers may then omit
nested cache locks. Generic cache paths, inode/accounting locks, all existing
reference/DMA/state checks and durability paths remain unchanged. The proof is
cleared before returning from the typed write. Full v2/v3 validation and validation cleanup passed before starting the
same-binary DB ABBAAB and16MiB-cache full payload/revision/reopen checks. All main and16MiB-cache full oracles/reopens and cleanup passed. Main long-batch medians were Level131203.41→131292.59 (+0.068%) and Rocks177769.60→178226.78 (+0.257%). Neither exceeds the predefined0.5% screen; REJECTED, not imported. The prepared confirmation is not queued.


Before clean-drain physical trials, the adoption screen was recorded: sequential
read median gain above0.5%, neither mixed75 nor4KiB read median losing more than
0.5%, and all validation/counter/cleanup checks passing. A passing result needs
reversed-order confirmation. Writer-owner uses the existing DB screen: one
long-batch median improves above0.5%, neither DB loses more than0.5%, and all
recovery/oracle/cache16/cleanup checks pass. These screens are candidate filters,
not a claim of statistical significance or measured XFS parity.


`thread-load-20261114` started after writer-owner PASS+cleanup. This diagnostic
uses the combined full-copy source without either new optimization overlay.
Existing SPDK `busy_tsc`/`idle_tsc` and bdev statistics are read only at shutdown,
with no per-I/O timing instrumentation. The previously validated opt-in readonly
four-file fio adapter allows identical files/80GiB and totalQD128 for one/four
threads. Full v2/v3 and multifile CRC checks precede the physical read-state
controls. Framework busy/idle accounting includes startup, teardown and lock
waits; it is not isolated instruction time or proof of a device ceiling.


clean-drain; PASS with cleanup. Same-binary median of three trials per arm.

| profile | OFF median (range) | ON median (range) | change |
| --- | ---: | ---: | ---: |
| seq-read | 26,495.65 (26,474.01..26,498.10) | 26,510.70 (26,509.30..26,532.29) | +0.06% |
| read4k-control | 1,702,802.52 (1,701,346.37..1,702,871.33) | 1,703,648.84 (1,702,700.93..1,703,845.42) | +0.05% |
| seq-mixed75 | 34,384.65 (34,360.59..34,404.74) | 34,379.35 (34,308.91..34,407.60) | -0.02% |


All118/119 clean read-period drains disappeared with the feature ON, while
mixed workload periodic dirty commits remained117/118 per invocation. The
predeclared performance screen failed: sequential read gained only0.06%,4KiB
read0.05%, mixed changed-0.02%. Full v2/v3 validation, wide physical CRC,
all18 measured profiles, per-arm conditioning, inode audits and cleanup passed.
No early termination or selective rerun occurred. Removing the observed no-op
periodic drain does not solve the remaining sequential-read gap.


## Thread-load diagnostic and calibration

`thread-load-20261114` finished PASS with cleanup after full v2/v3 and one-thread/four-file CRC validation. On identical four20GiB files and totalQD128, three-trial medians were one thread1,708,674 IOPS and four threads1,700,805 IOPS. Single QD256/512 controls were1,717,583/1,717,149 IOPS. This supports a shared-backend ceiling; increasing application threads or depth alone does not improve throughput here.

Shutdown SPDK framework FS busy was about99.9% for both thread counts; IO busy about71%. SPDK classifies a poll interval busy when its pollers return positive work. The sample includes startup, teardown, idle scanning inside a productive poll and waits: it does not prove instruction-level CPU saturation or identify the device ceiling. A balanced unchanged-binary calibration completed PASS+cleanup at unlimited,850k and425k total offered IOPS, three trials each. Actual medians1,702,411/849,913/424,956 IOPS corresponded to FS framework busy99.96/83.54/64.07% and IO71.74/47.92/28.87%. The counter responds to load; productive-poll accounting still does not establish isolated instruction saturation.

The timed-random-write control then read1,157,281 IOPS with105.13us aggregate bdev mean latency; explicit full80GiB sequential plus160GiB random rewrites with refreshed buffers restored1,701,807 IOPS and65.40us. These are repeated device/write-state effects, not isolated CPU or source deltas. No value is substituted into the standard matched XFS table.

The caller-cache experiment initially used a separate default-OFF `NVFUSE_API_CACHE_RETRY` overlay: after an initial caller-cache gate failure, retry for approximately10us before the original owner-dispatch fallback. All handle, live-client, data-failure and cache-validity checks remain under the original gate. Full v2/v3 pending-AIO/eviction/mutation/fault coverage precedes same-binary DB ABBAAB and both-arm16MiB stress. The predefined screen requires one long-batch gain above0.5%, neither DB long-batch nor warm-read median loss above0.5%, all oracles/reopens/cleanup PASS and reversed-order confirmation before adoption. It completed after rate-calibration; the confirmation and adoption decision are recorded below.


thread-load; PASS with cleanup. Shutdown SPDK framework and bdev counters include startup/teardown.

| condition | n | read IOPS median (range) | bdev mean us | FS framework busy % | IO framework busy % |
| --- | ---: | ---: | ---: | ---: | ---: |
| same-files-j4-d32 | 3 | 1,700,805 (1,700,060..1,702,418) | 65.34 | 99.96 | 71.84 |
| same-files-j1-d128 | 3 | 1,708,674 (1,698,224..1,709,040) | 66.03 | 99.91 | 70.93 |
| depth-j4-d64 | 1 | 1,717,583 (1,717,583..1,717,583) | 137.87 | 99.97 | 76.20 |
| depth-j4-d128 | 1 | 1,717,149 (1,717,149..1,717,149) | 283.60 | 99.95 | 76.22 |
| after-timed-random-read | 3 | 1,157,281 (1,155,920..1,158,645) | 105.13 | 99.18 | 61.37 |
| after-refill-full-random-read | 3 | 1,701,807 (1,701,698..1,701,984) | 65.40 | 99.96 | 71.67 |

rate-calibration; PASS with cleanup. Shutdown SPDK framework and bdev counters include startup/teardown.

| condition | n | read IOPS median (range) | bdev mean us | FS framework busy % | IO framework busy % |
| --- | ---: | ---: | ---: | ---: | ---: |
| rate-0 | 3 | 1,702,411 (1,700,551..1,702,525) | 65.46 | 99.96 | 71.74 |
| rate-850000 | 3 | 849,913 (849,912..849,914) | 59.57 | 83.54 | 47.92 |
| rate-425000 | 3 | 424,956 (424,955..424,956) | 56.24 | 64.07 | 28.87 |


## Bounded caller-cache retry experiment (adopted)

`cache-retry-20261114` passed full v2/v3 concurrency, pending-AIO timeout/fallback, crash/fault/recovery, CRC and runner validation with cleanup. Main same-binary ABBAAB, all payload/revision oracles and fresh-process reopens, both-arm/application16MiB cache stress and physical cleanup passed. Three-trial long-batch medians: Level130,984.60→131,119.79 (+0.10%), Rocks176,844.23→179,098.73 (+1.27%). Warm-read medians changed-0.16%/+0.01%, respectively. The predefined main screen passed.

The first Level ON invocation recorded84,688 contended entries:56,389 acquired the original gate within the bounded retry and28,299 fell back. Baseline queued fallback85,189 versus ON28,299 is mechanism evidence, not a phase-isolated or normalized throughput metric. Counters cover the whole process and DB phase traffic may differ.

`cache-retry-confirm-20261114` completed PASS+cleanup unchanged-binary reversed BAABBA with full oracles/reopens and both-arm/application16MiB stress. Its launch reverified the original source/library/app/result build hashes and enforced terminal validation/cleanup PASS and the predefined screen. Confirmed long-batch medians were Level130,791.03→131,426.02 (+0.49%) and Rocks177,161.13→178,951.65 (+1.01%), with warm-read controls+0.02%/+0.09%. Both orders passed the predefined screen; the bounded caller-cache retry is ADOPTED locally default ON, backout `NVFUSE_API_CACHE_RETRY=0`. No isolated percentage is added to the latest XFS table.


Read-latency postanalysis of the completed thread-load controls: median fio completion latency was74.93us versus aggregate bdev65.34us at4xQD32;148.72/137.87us at totalQD256;297.78/283.60us atQD512. Throughput remained about1.7M IOPS while bdev latency grew with depth. After timed random writes, fio/bdev means were110.25/105.13us; after full refreshed-buffer rewrites74.88/65.40us. The write-state slowdown appears inside the bdev/I/O-reactor interval as well, rather than only at the FS frontend. Bdev includes queueing/polling and startup metadata while fio is timed workload only, so subtracting these means is not a strict latency decomposition or proof of isolated hardware latency. The read stats and latency analysis are retained separately from candidate performance deltas.


`parity-caller-cache-20261114` completed PASS with cleanup: exact default-ON source rebuilt and fully v2/v3 validated, then28 DB invocations plus28 fresh-process reopens and50 fio invocations, including80GiB full CRC32C write/read per backend and actual inode/tail/EOF audits. Archive SHA256 `6ccaf22107c9fbec6231112ae15125684ef2ded2abbe5db142539014ad3359b6` has278 manifest entries and matched all140 C/H/inc files at capture; its local manifest SHA matched the remote build proof. At that checkpoint the XFS table used only that complete matched run. All three trials remain included despite LevelDB long-batch variation; no selective rerun or arithmetic addition of isolated gains.


Read-only live link check of the selected controller confirmed NUMA0 and current=max PCIe16.0GT/s x4 (Gen4x4), vendor/device1c5c:2b59. Its coding-only ceiling is7.877GB/s per direction before TLP/NVMe overhead. Observed sequential throughput around7.1GB/s and fast4KiB reads around7.0GB/s are already near that interface bandwidth. This further limits a CPU-only interpretation of the shared1.7M-IOPS ceiling; neither this theoretical ceiling nor productive-poll busy accounting independently proves the limiting component. No link setting was changed.


## Private RAM cache field-order candidate (rejected)

`cache-layout-20261114` is an isolated two-binary experiment based on the exact default-ON caller-cache source. It retains every private `nvfuse_buffer_cache` field and type, cache/hash/LRU algorithms, locks, references and disk layout. Only the in-memory order changes to place the loaded-page key/state/reference/physical/buffer fields in the first64 bytes. The original expected SPDK size/offsets are104/88/84/48/40 for size/buf/pno/lock/key; candidate96/56/48/64/32. These sizes do not by themselves establish reduced DPDK mempool allocation: per-object padding may dominate. Actual SPDK layout and sample alignment must pass before throughput trials.

Both separately built arms must pass full v2/v3 validation before main DB order `0,1,1,0,0,1,1,0,0,1`, five trials per arm/application, then both-arm/application16MiB cache stress with complete payload/revision oracles and fresh-process reopens. Five trials were chosen before upload because the preceding complete LevelDB1M results showed variation; all trials remain included. The screen requires one long-batch gain above0.5%, neither DB long-batch nor warm-read median loss above0.5%, all validations/oracles/reopens and cleanup PASS. This DB screen alone cannot authorize adoption: separate same-condition fio controls and reversed-order confirmation remain mandatory. Working implementation is unchanged by this candidate.


`cache-layout-20261114` completed both builds and full v2/v3 validation with cleanup, actual SPDK size/offset/alignment gates,20 main DB runs plus four16MiB stress runs, all24 fresh-process reopens and physical cleanup. Long-batch medians were Level131,392.25→131,861.04 (+0.36%) and Rocks179,069.55→179,332.62 (+0.15%); warm reads changed+0.41%/+0.47%. Neither long-batch gain exceeded the predefined0.5% threshold, so the candidate is REJECTED and was not imported. Performance rejection was knowable after18 main runs from median bounds; all remaining runs, cache16 checks and cleanup still completed. Prepared follow-up fio controls were never uploaded or queued.

cache-layout; PASS with cleanup. Two separately rebuilt binaries; median of five trials per arm.

| profile | Original median (range) | Hot order median (range) | change |
| --- | ---: | ---: | ---: |
| level load-100 | 266,258.24 (266,229.86..266,468.60) | 266,300.85 (264,875.75..266,807.27) | +0.02% |
| level random-read-warm | 178,842.93 (178,647.80..178,969.78) | 179,581.87 (178,445.51..179,856.68) | +0.41% |
| level update-1 | 35,968.05 (35,807.73..35,978.85) | 35,878.00 (35,850.56..35,940.39) | -0.25% |
| level update-100 | 131,392.25 (129,431.56..132,047.13) | 131,861.04 (130,366.15..132,378.51) | +0.36% |
| rocks load-100 | 221,955.08 (221,382.84..222,734.53) | 221,823.37 (221,268.26..222,171.22) | -0.06% |
| rocks random-read-warm | 180,498.22 (179,847.37..180,508.34) | 181,355.28 (180,595.01..182,243.24) | +0.47% |
| rocks update-1 | 39,663.86 (39,332.08..39,689.14) | 39,571.41 (39,404.45..39,677.82) | -0.23% |
| rocks update-100 | 179,069.55 (178,383.54..179,419.04) | 179,332.62 (178,126.80..179,657.09) | +0.15% |


## Bounded retry budget experiment (adopted; full refresh complete)

`retry-budget-20261114` separately compares10us versus50us on the exact current five-change source, with an experimental environment-selected budget and a shared binary. At the start of this isolated experiment the default remained10us; the later confirmed adoption below supersedes that checkpoint. The unadopted cache-layout overlay is absent. All original handle/live-client/data-failure/cache checks stay under the original gate; both arms retain bounded fallback to the queued owner. The hypothesis is that50us can reduce the remaining contended-read fallbacks during compaction; waiting longer may also waste CPU, so it must be measured.

The50us path must first pass the full v2/v3 suites including forced pending-AIO timeout accounting, mutation/eviction/crash/fault/recovery/CRC/runner checks. Main order `0,1,1,0,0,1,1,0,0,1` uses five trials per arm/application, followed by both-arm/application16MiB stress, full payload/revision oracles, fresh-process reopens and physical cleanup. The predeclared screen requires one long-batch median gain above0.5% and neither long-batch nor warm-read loss above0.5% in either DB. A passing result still requires unchanged-binary reversed confirmation before adoption and then exact-default full matched XFS revalidation; no isolated percentage can update the current XFS table.


## Historical10us checkpoint DB latency inspection

Matched current-source DB batch latency. Each transaction updates100 values; medians across the same three original trials.

| DB / update operations | XFS p50/p99 us | NVFUSE p50/p99 us | NVFUSE p99 change |
| --- | ---: | ---: | ---: |
| level / 100,000 | 405.76 / 1671.80 | 407.39 / 1703.33 | +1.89% |
| level / 1,000,000 | 410.36 / 1866.76 | 423.93 / 1937.73 | +3.80% |
| rocks / 100,000 | 491.00 / 1140.90 | 472.83 / 867.50 | -23.96% |
| rocks / 1,000,000 | 490.62 / 1134.50 | 472.71 / 984.24 | -13.24% |

The long RocksDB runs individually had mean transaction times558.20–560.17us for NVFUSE versus533.04–536.28us for XFS, despite lower NVFUSE p50/p95/p99. Percentiles alone do not establish whether the difference is in the distribution body, rarer tail or time outside the per-transaction timer. The post-timer diagnostic payload emits p01/p25/p75/p99.9/p99.99/max, seven latency-bin counts/sums, slowest1% sum and outside-transaction time from the latency vector already recorded. The summary runs only after the original phase timer stopped and its existing sort; transaction code, original timers and I/O semantics are unchanged. The prepared focused diagnostic covers LevelDB100k and RocksDB1M, one smoke and three full runs per application/backend, all payload/revision oracles and reopens. It reuses and hashes the current validated runtime library before/after the app-only build, and was launched as `batch-tail-20261114` only after the exact-default50us full matched comparison and both validation settings passed with cleanup. It is a distribution diagnostic, not an implementation-performance comparison.


`retry-budget-20261114` completed PASS+cleanup: full50us v2/v3 including forced timeout accounting,20 main DB runs, four16MiB stress runs, all24 full-oracle/revision reopens and physical restore. The predefined main screen passed: long-batch Level+0.465%, Rocks+0.689%; warm-read Level-0.483%, Rocks-0.163%. Level single-update throughput decreased1.413%, while Rocks single-update increased0.662%; these tradeoffs must remain visible. The unchanged-binary reversed `BAABBAABBA` five/arm confirmation subsequently passed after rechecking original source/library/app/result hashes, as recorded below. At this main-screen checkpoint the default was still10us; adoption followed confirmation. No isolated gain updates the XFS table.

retry-budget; PASS with cleanup. Same-binary median of five trials per arm.

| profile | 10us median (range) | 50us median (range) | change |
| --- | ---: | ---: | ---: |
| level load-100 | 265,772.03 (265,318.10..265,904.48) | 265,748.07 (265,553.60..265,918.66) | -0.01% |
| level random-read-warm | 178,905.68 (178,376.63..179,391.53) | 178,041.06 (177,768.33..179,224.04) | -0.48% |
| level update-1 | 35,809.64 (35,660.57..35,949.46) | 35,303.66 (34,838.93..35,447.82) | -1.41% |
| level update-100 | 131,109.33 (130,107.24..131,512.25) | 131,719.32 (130,256.81..132,250.28) | +0.47% |
| rocks load-100 | 220,792.31 (220,318.16..220,995.82) | 221,010.03 (220,146.00..221,102.47) | +0.10% |
| rocks random-read-warm | 180,408.17 (179,597.79..181,532.09) | 180,114.62 (179,693.76..180,736.90) | -0.16% |
| rocks update-1 | 39,501.34 (39,341.08..39,589.51) | 39,762.70 (39,581.03..39,860.85) | +0.66% |
| rocks update-100 | 178,531.11 (178,033.45..179,221.41) | 179,761.89 (177,213.98..180,129.50) | +0.69% |


`retry-budget-confirm-20261114` finished PASS+cleanup with all20 main runs, four16MiB stress runs and24 full-oracle fresh-process reopens. Reversed long-batch changes were Level+0.657% and Rocks+0.673%; warm reads-0.156%/-0.196%. Both original and reversed orders pass the predefined screen. Level single updates decreased1.493% in confirmation (main-1.413%), while Rocks single updates increased0.839% (main+0.662%). This is an explicit batch-versus-single-update tradeoff, not a uniform speedup.

The bounded gate budget is adopted locally at default50us, with `NVFUSE_API_CACHE_RETRY_US=10` restoring the previous budget and `NVFUSE_API_CACHE_RETRY=0` restoring immediate queued fallback. Only10/50us are supported. All gate/client/data-failure/cache validity checks and bounded queued fallback remain; journal/fsync semantics are unchanged. The exact new source archive `9728549999cde4cf5f5e62f22b030ef997764610b7bc7fb030b204409c506f75` has278 manifest entries and matches140 current C/H/inc files. `parity-retry-budget-20261114` was configured to run full v2/v3 on both actual default50us and the10us override, then the complete matched28 DB/50fio comparison with all CRC/oracle/reopen/inode/tail/EOF/cleanup checks. The XFS table now uses the complete50us refresh.

retry-budget-confirm; PASS with cleanup. Same-binary median of five trials per arm.

| profile | 10us median (range) | 50us median (range) | change |
| --- | ---: | ---: | ---: |
| level load-100 | 265,938.66 (265,070.68..266,273.91) | 266,020.98 (265,375.50..266,644.93) | +0.03% |
| level random-read-warm | 178,417.28 (177,916.62..178,804.75) | 178,139.60 (177,787.82..178,965.60) | -0.16% |
| level update-1 | 35,808.44 (35,567.37..35,814.59) | 35,274.04 (35,243.68..35,496.00) | -1.49% |
| level update-100 | 130,581.52 (129,806.28..131,365.15) | 131,439.12 (130,641.30..132,099.98) | +0.66% |
| rocks load-100 | 220,743.89 (219,893.87..220,967.87) | 220,903.02 (220,572.31..220,944.09) | +0.07% |
| rocks random-read-warm | 180,709.74 (179,741.76..181,134.72) | 180,355.81 (179,991.90..181,479.26) | -0.20% |
| rocks update-1 | 39,472.02 (39,425.64..39,575.57) | 39,803.13 (39,609.79..39,819.32) | +0.84% |
| rocks update-100 | 178,224.88 (177,726.92..179,248.35) | 179,424.62 (178,628.14..179,987.62) | +0.67% |


`parity-retry-budget-20261114` completed PASS with cleanup. Build and exact-default50us plus10us-override full v2/v3 validation passed, followed by28 DB invocations and28 fresh-process reopens,50 fio invocations including full80GiB CRC32C write/read per backend, all inode/tail/EOF audits and physical restore. The current archive9728549999cde4cf5f5e62f22b030ef997764610b7bc7fb030b204409c506f75 matches all140 current C/H/inc files;278-entry manifest hash exactly matches the remote build proof. The latest XFS table uses only this complete run; no isolated delta was added. Primary4KiB read uses the user-selected same explicit preceding write volume; time-based write history remains a secondary result.


`batch-tail-20261114` was launched after the complete50us refresh and both validation settings passed with cleanup. It rebuilds application reporting only and verifies unchanged runtime/library hashes before and after that build. LevelDB100k and RocksDB1M each use one smoke plus three complete trials per backend (16 DB invocations and16 full payload/revision reopens). No fio, fsync or transaction-loop change is part of this diagnostic. All16 DB invocations,16 fresh-process full payload/revision reopens and physical cleanup passed.


## Completed post-timer distribution diagnosis

Post-timer latency diagnostic using the current default50us runtime.
Three complete trials per application/backend; original sample timing and workload are unchanged.
These diagnostic results do not replace the complete matched XFS table.

|Application/backend|Median ops/s|Mean phase s|Body99% s|Top1% s|Outside transactions s|p50 / p99 / p99.9 us|Maximum us|
|---|---:|---:|---:|---:|---:|---:|---:|
|level/xfs|163,675.85|0.610980|0.592720|0.018164|0.000096|407.06 / 1662.94 / 2003.09|2126.02|
|rocks/xfs|187,416.76|5.340617|5.175091|0.163523|0.002003|489.66 / 1122.24 / 2610.22|8278.91|
|level/nvfuse|142,637.27|0.706480|0.679434|0.026951|0.000095|405.71 / 1744.57 / 4160.67|5163.99|
|rocks/nvfuse|179,239.70|5.567675|5.176781|0.389712|0.001181|471.54 / 904.41 / 6837.72|7426.13|

Time accounting uses arithmetic means across all three runs, so body + tail + outside equals the mean phase time.
Quantiles use the median of run quantiles; maximum is the largest recorded sample.

|Application|NVFUSE-XFS phase s|Body99% s|Top1% s|Outside s|
|---|---:|---:|---:|---:|
|level|+0.095500|+0.086715|+0.008787|-0.000001|
|rocks|+0.227058|+0.001691|+0.226189|-0.000822|

|Application/backend|<500us s|500-1000us s|1-2ms s|2-5ms s|5-10ms s|10-100ms s|>=100ms s|
|---|---:|---:|---:|---:|---:|---:|---:|
|level/xfs|0.320311|0.035425|0.251705|0.003443|0.000000|0.000000|0.000000|
|rocks/xfs|3.678801|1.330990|0.281705|0.032918|0.014199|0.000000|0.000000|
|level/nvfuse|0.284863|0.020768|0.377442|0.021591|0.001721|0.000000|0.000000|
|rocks/nvfuse|3.194611|2.002630|0.026144|0.041222|0.301887|0.000000|0.000000|

RocksDB long-batch mean excess227.058ms is almost entirely in the slowest1% (+226.189ms); the other99% differs by only+1.691ms. The5-10ms bin totals301.887ms versus14.199ms for XFS. LevelDB short-batch mean excess95.500ms is mainly in the other99% (+86.715ms), not its slowest1% (+8.787ms). Outside-transaction overhead is negligible in both. These distinguish two bottlenecks but do not identify whether RocksDB write stalls, periodic journal work or another mechanism causes the5-10ms transactions. Next diagnostic will correlate existing transaction windows with periodic journal spans and DB stall statistics; no implementation change is selected yet.


`periodic-trace-20261114` built a private diagnostic overlay after batch-tail PASS+cleanup. It records periodic journal start/end times with CLOCK_MONOTONIC, slow transaction windows using the existing transaction clocks, and post-phase RocksDB cfstats. The100ms journal deadline, default50us retry, workload, fsync and I/O policy are unchanged; instrumentation overhead applies. Full v2/v3 validation passed before16 application/backend invocations and reopens, which also passed with cleanup. Production source is unchanged, and the complete matched table remains the uninstrumented50us run.


## Periodic-journal correlation diagnostic completed

`periodic-trace-20261114` passed full v2/v3 validation,16 DB invocations and16 full-oracle fresh-process reopens, and physical cleanup. RocksDB5-10ms transactions had only84.061/51.811/25.645us total overlap with periodic commits in the three NVFUSE trials; total periodic commit time across each whole process was below0.727ms. Periodic commit execution cannot explain the hundreds of milliseconds in those transactions. The post-phase CF statistics changed from zero to17 L0-file-count-limit delays in each NVFUSE trial, versus11/13/13 for XFS. No memtable/pending-compaction-byte stops/delays were recorded. This supports a compaction-throughput/write-delay mechanism, while per-instruction attribution still requires an isolated optimization experiment. Original statistics and temporal overlaps are in `periodic-trace-analysis.json/md`; no diagnostic throughput updates the latest matched table.

## Cached-read LRU head experiment (rejected)

The next private default-OFF overlay skips `list_move` only when a loaded-page read already finds that page at the head of its existing LRU list. Both inode and data touches remain under the original filesystem gate and validity checks. The final list order is identical; no references, mappings, cache capacity, writeback, journal or fsync policy changes. This targets remaining compaction read-path overhead identified in per-TID profiles; a benefit is unproven. The uninstrumented current source and100ms journal/default50us retry remain the baseline.

Predeclared screen: main ABBAAB three trials per arm for both100k and1M batch lengths in both DBs, plus both-arm/application16MiB cache integrity stress. At least one batch median gain must exceed0.5%, no batch or warm-read median loss may exceed0.5%, and no single-update median loss may exceed1%. All validation, actual-use counters, full payload/revision oracles, reopens and cleanup must pass. A passing screen requires unchanged-binary reversed confirmation before adoption, then a new exact-default matched XFS comparison. All results are retained; no isolated gain is added to the XFS table.

`cache-head-20261114` completed with the default-OFF overlay isolated from production. Production source remains the completed50us baseline. Its builder required prior complete-run and periodic-trace terminal PASS+cleanup, and both arm settings passed full v2/v3 validation before physical A/B.

After21/24 main invocations, the cache-head performance screen is already unattainable: all OFF medians are complete; the final ON median in each two-sample group must lie between the existing values. Maximum possible batch gains are LevelDB short-0.286%, LevelDB long+0.177%, RocksDB short+0.094% and RocksDB long+0.396%. Thus no batch can exceed the predeclared0.5% threshold. REJECT for adoption; finish every remaining main/cache16 invocation, full oracle/reopen and cleanup without early termination. Prepared reversed confirmation remains local only and will not be queued.


`cache-head-20261114`: PASS integrity/reopens/cleanup; performance screen FAIL.
Same-binary median of three trials per arm; all measured trials retained.

|App/scenario/phase|OFF median (range)|ON median (range)|Change|
|---|---:|---:|---:|
|level/100000/load-100|268,190.03 (267,630.32–268,801.95)|267,796.90 (256,721.21–268,651.65)|-0.147%|
|level/100000/random-read-warm|180,665.63 (180,301.46–180,856.10)|180,844.48 (180,361.11–181,042.49)|+0.099%|
|level/100000/update-1|35,553.56 (35,436.42–35,664.59)|35,753.32 (35,061.59–35,799.91)|+0.562%|
|level/100000/update-100|144,493.16 (144,156.69–147,132.74)|144,080.07 (139,031.34–144,432.60)|-0.286%|
|level/1000000/load-100|268,188.29 (266,986.94–269,502.67)|268,240.35 (267,571.13–268,254.90)|+0.019%|
|level/1000000/random-read-warm|181,023.65 (179,931.07–181,155.53)|180,910.73 (180,645.02–181,657.03)|-0.062%|
|level/1000000/update-1|35,617.10 (35,459.99–35,658.03)|35,585.13 (35,553.58–35,654.57)|-0.090%|
|level/1000000/update-100|135,929.94 (128,417.88–135,980.62)|135,645.72 (135,589.44–136,170.95)|-0.209%|
|rocks/100000/load-100|221,573.79 (219,913.15–221,702.80)|221,732.58 (220,497.15–222,155.87)|+0.072%|
|rocks/100000/random-read-warm|182,259.00 (182,131.92–183,909.32)|183,422.24 (183,176.75–183,998.45)|+0.638%|
|rocks/100000/update-1|40,475.41 (39,833.44–40,495.12)|40,345.04 (40,318.94–40,446.30)|-0.322%|
|rocks/100000/update-100|186,203.76 (180,997.18–189,004.47)|186,051.52 (184,935.97–186,378.92)|-0.082%|
|rocks/1000000/load-100|221,584.89 (219,950.63–222,620.48)|221,613.81 (220,649.53–221,836.95)|+0.013%|
|rocks/1000000/random-read-warm|183,832.09 (183,208.68–185,086.16)|183,120.76 (182,870.86–183,588.81)|-0.387%|
|rocks/1000000/update-1|40,407.01 (40,206.86–40,424.42)|40,475.13 (40,272.68–40,536.62)|+0.169%|
|rocks/1000000/update-100|182,444.74 (179,576.50–182,453.08)|182,387.62 (182,221.75–183,166.96)|-0.031%|

Screen conditions:
- at_least_one_batch_gain_over_half_percent: False
- no_batch_loss_over_half_percent: True
- no_warm_loss_over_half_percent: True
- no_single_loss_over_one_percent: True

Actual-use counts cover each whole process, not an isolated DB phase. All ON main and cache16 runs exceeded1,000 skipped redundant head moves; all OFF counts were zero. No isolated percentage updates the complete matched XFS table.

Both OFF/ON full v2/v3 suites,24 main+4 cache16 invocations and all28 fresh-process full payload/revision reopens passed with physical cleanup. No early termination occurred. The optimization is REJECTED and is absent from production source; locally prepared confirmation was never uploaded or queued.


## Read-copy calibration and current-binary profiling

Historical pre-full-copy instruction annotation places most sampled `nvfio_pread` self cost on `rep movsq`. It combines threads because this server's `perf annotate` lacks TID filtering; these percentages are not current-source measurements. Disassembly independently confirms current exact50us `nvfio_pread` still uses `rep movsq`.

`copy-calibration-20261114` passed18 CPU-only trials and payload/guard checks under the benchmark lock, with the independent soak checked before/after. CPU10 compared4KiB inline `rep movsq` with a volatile-function-pointer libc `memcpy`, ABBAAB three trials/arm at each working-set size. No device I/O, runtime change or filesystem performance measurement occurred.

|Working set MiB|Inline median GiB/s|libc median GiB/s|Change|
|---|---:|---:|---:|
|2|68.341|68.024|-0.464%|
|128|15.906|15.899|-0.041%|
|512|14.232|14.210|-0.156%|

There is no consistent benefit for this aligned full4KiB case; no implementation was added. This narrow calibration does not cover the partial-page sizes or source/destination alignments that DB block reads can produce, so it does not rule out a separately justified copy strategy for those shapes. Raw evidence and analysis are preserved in `copy-calibration-analysis.json` and the two remote-evidence directories.

`current-batch-profile-20261114` samples only the batch-update interval on the unchanged, hash-verified current default50us binaries: LevelDB100k and RocksDB1M, with smoke/full runs and full-oracle fresh-process reopens. No rebuild or overlay is involved. Existing device identity/unused/SMART/soak guards, selected driver/override/actual hugepage restoration remain required. Thread-specific attribution and marker lag/profiling overhead must be retained; diagnostic throughput cannot update the matched XFS table.


`current-batch-profile-20261114` completed PASS: four smoke/full DB invocations, four full-oracle fresh-process reopens, unchanged source/library/application hashes and physical cleanup. Unprivileged `/proc/<root process>/exe` access prevented collecting the thread tick snapshots, so CPU utilization cannot be inferred from this stage. The initially empty snapshot analysis is retained as `post-analysis-empty-proc.json`; corrected analysis recovers TIDs directly from perf sample headers and requires nonempty function reports. No device workload was repeated.

The short LevelDB batch interval has only2,418 total samples (663 on its background thread), so its exact-TID self-cost estimates are coarse: `nvfio_pread`13.67%, `nvfuse_append_cached_page`12.95%, hash lookup7.46%. RocksDB has22,408 total samples; on `rocksdb:low`, cycle-weighted exact-TID relative self reports show `__pthread_mutex_trylock`17.16%, `nvfio_pread`14.63% and XXH3 hashing11.83%. `rocksdb:high` has882 samples and49.72% self in `nvfuse_write_mapped_cache`. These are per-thread profile proportions, not overall elapsed-time or current XFS deltas.

A symbol-filtered perf caller graph rendered some other-thread stacks, so exclusive attribution instead parses the actual sample-header TID and DWARF stack together. On RocksDB low TID154459,681 of699 mutex-leaf sample stacks include `nvfio_pread`; one includes `nvfio_pwrite`,17 are other/unresolved. This supports reducing contention-probe overhead while preserving the already validated retry budget. Raw samples, original and corrected analysis, and direct stack proof are preserved.

## Cached-read retry cadence experiment

`retry-cadence-20261114` is an isolated default-OFF runtime overlay. The candidate retains the immediate first trylock, the50us bounded cache retry,16-pause deadline-check quantum, filesystem mutex, all handle/client/data-failure/cache checks and queued fallback. Only already-contended retry probes change from every pause to every eighth pause. The existing general gate, I/O, cache capacity, journal deadline and fsync rules are unchanged. Suppressed-probe counts prove actual use; no implementation is adopted yet.

Predeclared before upload: both OFF/ON full v2/v3 validation, then same-binary ABBAAB three trials/arm for LevelDB and RocksDB at100k/1M batch lengths, plus both-arm/application16MiB cache stress. Require at least one batch median gain>0.5%, no batch/warm-read median loss>0.5%, no single-update median loss>1%, all full payload/revision oracles/reopens, actual-use counters and physical cleanup PASS. Complete all trials even if failure becomes certain. A passing screen requires unchanged-binary reversed confirmation before adoption and a new exact-source matched XFS comparison afterward. Isolated gains never update the complete XFS table.


After19/24 main invocations, the retry-cadence performance screen is unattainable. LevelDB1M OFF values125177.88/132073.90/132642.97 give median132073.90; the two ON values120195.59/129996.23 bound any final ON median at129996.23 or lower. Thus even the best possible final median loses1.573%, exceeding the predeclared0.5% loss limit. REJECT for adoption; complete all remaining main/cache16 runs, full oracles/reopens and cleanup without early termination. Prepared reversed confirmation remains local only and will not be uploaded or queued.


`retry-cadence-20261114`: PASS integrity/reopens/cleanup; performance screen FAIL.
Same-binary median of three trials per arm; all measured trials retained.

|App/scenario/phase|OFF median (range)|ON median (range)|Change|
|---|---:|---:|---:|
|level/100000/load-100|267,427.98 (266,914.86–267,843.24)|265,680.24 (263,787.14–266,816.87)|-0.654%|
|level/100000/random-read-warm|178,926.28 (177,380.96–179,050.07)|178,103.50 (177,877.22–178,160.15)|-0.460%|
|level/100000/update-1|35,389.85 (35,379.92–35,557.05)|35,376.01 (35,237.47–35,388.62)|-0.039%|
|level/100000/update-100|143,954.35 (143,178.34–144,610.99)|153,016.40 (146,154.83–153,765.94)|+6.295%|
|level/1000000/load-100|267,211.08 (266,625.37–267,248.03)|267,325.38 (258,452.87–267,457.55)|+0.043%|
|level/1000000/random-read-warm|178,085.92 (177,824.94–178,181.35)|177,458.61 (175,489.41–178,355.13)|-0.352%|
|level/1000000/update-1|35,456.09 (35,265.37–35,533.99)|36,246.72 (35,264.58–36,544.11)|+2.230%|
|level/1000000/update-100|132,073.90 (125,177.88–132,642.97)|129,172.11 (120,195.59–129,996.23)|-2.197%|
|rocks/100000/load-100|222,104.46 (220,806.67–222,369.87)|221,299.64 (220,538.91–221,445.32)|-0.362%|
|rocks/100000/random-read-warm|180,923.80 (180,262.29–182,112.34)|180,436.43 (179,395.69–180,998.92)|-0.269%|
|rocks/100000/update-1|39,767.60 (39,710.95–39,946.26)|39,910.09 (39,832.79–40,065.86)|+0.358%|
|rocks/100000/update-100|187,394.52 (183,089.66–190,085.61)|189,137.79 (187,524.29–189,709.85)|+0.930%|
|rocks/1000000/load-100|221,388.16 (220,614.32–222,455.55)|221,492.63 (220,419.55–221,644.83)|+0.047%|
|rocks/1000000/random-read-warm|180,629.78 (180,077.35–180,916.34)|180,914.72 (180,441.17–181,000.68)|+0.158%|
|rocks/1000000/update-1|39,685.66 (39,659.73–39,873.90)|39,908.29 (39,843.92–39,957.13)|+0.561%|
|rocks/1000000/update-100|180,308.08 (176,107.44–180,459.70)|179,153.85 (178,954.78–180,301.49)|-0.640%|

Screen conditions:
- at_least_one_batch_gain_over_half_percent: True
- no_batch_loss_over_half_percent: False
- no_warm_loss_over_half_percent: True
- no_single_loss_over_one_percent: True

Actual-use counts cover each whole process, not an isolated DB phase. All ON main and cache16 runs exceeded1,000 suppressed retry probes; all OFF counts were zero. No isolated percentage updates the complete matched XFS table.

Both OFF/ON full v2/v3 suites,24 main plus4 cache16 invocations and all28 fresh-process full payload/revision reopens passed with physical cleanup and unchanged before/after artifact hashes. No early termination occurred. REJECTED, not imported; prepared reverse confirmation was never uploaded or queued.


## Partial and misaligned read-copy strategy

The aligned full4KiB calibration did not cover DB partial-page and mismatched-alignment copies. `copy-shapes-20261114` completed42 CPU-only ABBAAB trials,128MiB source, CPU10, source/destination payload and exact-boundary guard oracles. Disassembly asserts that the bounded compiler copy uses `rep movsq`. This is synthetic CPU calibration, not the actual DB copy-shape distribution or filesystem throughput.

|Synthetic copy shape|Compiler median GiB/s|libc median GiB/s|Change|
|---|---:|---:|---:|
|4096bytes aligned|15.803|15.849|+0.29%|
|4096bytes destination+1|7.124|13.892|+95.01%|
|4095bytes source+1|7.122|14.094|+97.88%|
|2048bytes aligned|14.501|13.955|-3.76%|
|2048bytes destination+3|5.210|13.040|+150.26%|
|Random page suffix/destination alignment|5.346|12.338|+130.80%|
|Random64–1087byte request, page clamp/alignment|2.186|9.193|+320.57%|

`read-copy-20261114` is a new isolated default-OFF candidate. Only loaded-page caller-cache read copies with n>=256 and differing source/destination8byte alignment use libc through a volatile function pointer. Original small/aligned copies remain. The exclusive filesystem gate, cache validity, live-AIO/data-failure checks, references/LRU, cache capacity, retry50us policy and I/O/journal/fsync rules remain unchanged. Runtime copied-call/byte counts prove actual use and bounded sizes. No code is adopted yet.

A private test overlay adds exact destination guards for lengths0/1/7/8/63/255/256/257/4095/4096/4097/8192, source offsets around page/EOF boundaries, and five destination shifts. Both OFF/ON full v2/v3 validation must pass this oracle before same-binary ABBAAB three trials/arm for both apps and100k/1M batch lengths, plus both-arm/app16MiB cache full-oracle/reopen stress. The predeclared screen is unchanged: one batch gain>0.5%, no batch/warm loss>0.5%, no single-update loss>1%, all validation/actual-use/full payload/revision/reopen/cleanup checks PASS. A passing screen requires unchanged-binary reversed confirmation before adoption and then a new exact-source matched XFS comparison. All trials are retained and completed.


Read-only ELF inspection found an instrumentation-layout confound in `read-copy-20261114`: `cache_read_libc` starts at0xfece40 but occupies only24bytes; `journal_interval`0xfece58 and `journal_deadline`0xfece60 share its64byte cacheline. Variable-only alignment did not pad the counter structure tail. This can make counter writes interfere with the owner poller's timer reads; actual causal cost is unmeasured. Complete and retain every original trial/oracle/reopen/cache16/cleanup; do not edit active binaries or adopt from this run.

A separate `read-copy-control-20261114` payload is prepared locally only. It changes the diagnostic structure to have64byte type alignment and size, with a compile-time assertion, and requires actual ELF counter/timer/gate line-separation proofs for both DB binaries before any timed comparison. The read-copy algorithm, threshold, OFF/ON selection and full validation/screen/confirmation requirements remain identical. This control will only start after original terminal PASS+cleanup; no selective original throughput reruns.


`read-copy-20261114`: PASS integrity/reopens/cleanup; performance screen PASS.
Same-binary median of three trials per arm; all measured trials retained.

|App/scenario/phase|OFF median (range)|ON median (range)|Change|
|---|---:|---:|---:|
|level/100000/load-100|268,286.30 (267,748.82–268,627.97)|267,529.79 (259,400.95–268,424.46)|-0.282%|
|level/100000/random-read-warm|180,905.60 (180,163.81–182,019.97)|188,230.99 (188,128.28–188,616.71)|+4.049%|
|level/100000/update-1|35,547.86 (35,478.42–35,618.91)|35,734.44 (35,304.52–35,826.29)|+0.525%|
|level/100000/update-100|144,102.42 (144,085.49–147,039.24)|145,814.67 (137,658.08–147,508.54)|+1.188%|
|level/1000000/load-100|267,947.64 (265,063.50–268,713.41)|267,558.76 (247,412.22–268,060.42)|-0.145%|
|level/1000000/random-read-warm|180,851.34 (180,475.71–181,063.35)|188,635.74 (188,592.30–188,753.62)|+4.304%|
|level/1000000/update-1|35,475.13 (35,472.30–35,720.11)|35,801.09 (35,357.55–35,850.18)|+0.919%|
|level/1000000/update-100|135,954.16 (128,565.50–136,264.54)|139,440.56 (127,126.75–140,066.60)|+2.564%|
|rocks/100000/load-100|221,867.23 (219,691.61–222,206.97)|222,116.70 (222,042.04–222,293.48)|+0.112%|
|rocks/100000/random-read-warm|183,594.10 (183,268.17–183,842.30)|190,642.58 (190,040.11–191,677.43)|+3.839%|
|rocks/100000/update-1|40,432.95 (40,007.12–40,562.14)|40,553.95 (40,481.69–40,595.45)|+0.299%|
|rocks/100000/update-100|184,864.13 (180,306.99–185,853.83)|185,681.17 (184,350.78–185,729.55)|+0.442%|
|rocks/1000000/load-100|221,791.61 (220,183.07–221,962.32)|222,025.10 (221,090.95–222,044.92)|+0.105%|
|rocks/1000000/random-read-warm|182,784.24 (180,993.50–183,591.06)|190,817.27 (190,521.48–191,042.07)|+4.395%|
|rocks/1000000/update-1|40,400.06 (40,095.65–40,453.79)|40,500.65 (40,475.90–40,580.82)|+0.249%|
|rocks/1000000/update-100|182,551.66 (178,784.31–183,071.51)|185,358.88 (185,149.13–185,529.90)|+1.538%|

Screen conditions:
- at_least_one_batch_gain_over_half_percent: True
- no_batch_loss_over_half_percent: True
- no_warm_loss_over_half_percent: True
- no_single_loss_over_one_percent: True

Actual-use counts cover each whole process, not an isolated DB phase. All ON main and cache16 runs exceeded1,000 conditional external copies; all OFF counts were zero. No isolated percentage updates the complete matched XFS table.

The original run completed both-arm full v2/v3 plus exact copy-guard oracles,24 main+4 cache16 invocations,28 full payload/revision reopens, unchanged artifact hashes and physical cleanup. All performance screen conditions pass, but it is NOT ADOPTED because of the separately confirmed diagnostic counter/timer shared cacheline. No original reversed confirmation was uploaded or queued. `read-copy-control-20261114` was launched only after this terminal result/cleanup; production remains the prior exact50us matched source.


The control builder passed actual ELF checks for both DB binaries: the counter structure is64bytes and its cacheline differs from both journal timers and `fs_gate`. Both OFF/ON full v2/v3 suites and copy-guard oracles passed with cleanup before the control physical ABBAAB began. No production adoption or XFS-table update has occurred.


## Corrected read-copy control and reversed confirmation

`read-copy-control-20261114`: PASS integrity/reopens/cleanup; performance screen PASS.
Same-binary median of three trials per arm; all measured trials retained.

|App/scenario/phase|OFF median (range)|ON median (range)|Change|
|---|---:|---:|---:|
|level/100000/load-100|268,889.37 (268,335.57–269,166.25)|266,984.55 (259,523.65–270,134.62)|-0.708%|
|level/100000/random-read-warm|181,587.14 (181,435.89–181,672.35)|190,746.18 (190,456.24–191,353.37)|+5.044%|
|level/100000/update-1|35,640.69 (35,608.93–35,860.44)|35,745.79 (35,635.24–35,950.81)|+0.295%|
|level/100000/update-100|144,871.33 (143,884.35–147,823.61)|147,012.92 (139,694.64–151,239.50)|+1.478%|
|level/1000000/load-100|268,615.98 (266,190.28–269,147.26)|267,236.11 (252,225.47–269,759.56)|-0.514%|
|level/1000000/random-read-warm|180,821.84 (180,768.33–181,811.54)|191,970.17 (190,904.21–192,013.96)|+6.165%|
|level/1000000/update-1|35,583.78 (35,502.07–35,696.78)|35,878.51 (35,696.23–35,930.50)|+0.828%|
|level/1000000/update-100|136,165.58 (128,705.45–136,495.33)|141,141.15 (129,474.77–141,195.13)|+3.654%|
|rocks/100000/load-100|222,290.72 (220,950.07–223,140.71)|221,301.93 (221,061.61–222,092.71)|-0.445%|
|rocks/100000/random-read-warm|183,943.58 (182,600.34–184,236.76)|193,434.07 (193,413.28–193,758.84)|+5.159%|
|rocks/100000/update-1|40,526.66 (40,149.73–40,562.37)|40,557.79 (40,508.89–40,562.17)|+0.077%|
|rocks/100000/update-100|184,749.49 (179,779.72–186,671.26)|185,056.75 (183,261.44–185,608.76)|+0.166%|
|rocks/1000000/load-100|221,458.85 (220,046.32–222,280.32)|221,251.75 (221,184.43–222,208.82)|-0.094%|
|rocks/1000000/random-read-warm|182,798.95 (182,271.51–183,087.32)|194,298.05 (193,670.64–194,905.38)|+6.291%|
|rocks/1000000/update-1|40,351.28 (40,099.87–40,484.64)|40,611.94 (40,558.28–40,704.72)|+0.646%|
|rocks/1000000/update-100|183,012.32 (178,483.50–183,047.59)|184,291.11 (183,272.30–184,403.26)|+0.699%|

Screen conditions:
- at_least_one_batch_gain_over_half_percent: True
- no_batch_loss_over_half_percent: True
- no_warm_loss_over_half_percent: True
- no_single_loss_over_one_percent: True

Actual-use counts cover each whole process, not an isolated DB phase. All ON main and cache16 runs exceeded1,000 conditional external copies; all OFF counts were zero. No isolated percentage updates the complete matched XFS table.

The corrected control completed both OFF/ON full v2/v3 suites and exact copy guards,24 main plus4 cache16 runs,28 full-oracle fresh-process reopens, actual ELF counter/timer/gate line separation, unchanged before/after hashes and physical cleanup. All predefined performance conditions passed. Load medians were also retained (LevelDB -0.708%/-0.514%, RocksDB -0.445%/-0.094%); load was not a predefined screen condition.

At launch, `read-copy-control-confirm-20261114` was configured for reversed BAABBA three/arm scenarios with unchanged binaries, no rebuild. It inherits the main full-validation proof, rechecks original source/library/app/result hashes and actual counter-layout proof, and repeats24 main+4 cache16 invocations with all payload/revision oracles and fresh-process reopens. Production source remains the previously adopted default50us source; adoption required terminal confirmation screen and cleanup PASS; final rejection is recorded below.


`read-copy-control-confirm-20261114`: PASS integrity/reopens/cleanup; performance screen FAIL.
Same-binary median of three trials per arm; all measured trials retained.

|App/scenario/phase|OFF median (range)|ON median (range)|Change|
|---|---:|---:|---:|
|level/100000/load-100|268,780.08 (267,889.78–269,480.14)|268,331.13 (267,751.90–268,952.84)|-0.167%|
|level/100000/random-read-warm|181,768.70 (181,551.09–181,771.48)|190,674.08 (190,324.38–190,997.05)|+4.899%|
|level/100000/update-1|35,547.75 (35,545.18–35,680.25)|35,704.15 (35,682.45–35,734.22)|+0.440%|
|level/100000/update-100|147,875.75 (146,866.79–147,957.74)|146,523.93 (145,142.09–146,846.55)|-0.914%|
|level/1000000/load-100|268,500.36 (267,281.71–269,016.54)|268,260.70 (261,308.18–269,375.31)|-0.089%|
|level/1000000/random-read-warm|180,794.60 (179,878.76–181,595.40)|190,500.05 (190,124.45–190,599.90)|+5.368%|
|level/1000000/update-1|35,663.97 (35,514.59–35,666.31)|35,696.62 (35,577.37–35,788.55)|+0.092%|
|level/1000000/update-100|136,384.94 (136,161.70–136,863.18)|140,540.76 (133,095.68–140,997.52)|+3.047%|
|rocks/100000/load-100|221,769.30 (221,336.79–222,597.57)|222,558.67 (221,191.80–223,279.12)|+0.356%|
|rocks/100000/random-read-warm|183,459.59 (181,404.42–184,060.59)|194,715.60 (192,633.80–195,187.80)|+6.135%|
|rocks/100000/update-1|40,478.49 (40,094.92–40,578.14)|40,632.33 (40,608.66–40,676.17)|+0.380%|
|rocks/100000/update-100|185,249.97 (184,698.69–185,816.27)|184,308.12 (184,169.23–185,043.34)|-0.508%|
|rocks/1000000/load-100|221,713.66 (221,679.74–222,495.01)|222,098.90 (220,927.20–222,501.86)|+0.174%|
|rocks/1000000/random-read-warm|183,981.04 (183,513.51–184,266.13)|194,733.62 (193,742.85–195,003.55)|+5.844%|
|rocks/1000000/update-1|40,529.07 (40,433.98–40,549.68)|40,638.37 (40,546.77–40,640.96)|+0.270%|
|rocks/1000000/update-100|182,154.60 (181,917.76–182,719.48)|184,273.68 (184,232.63–184,419.97)|+1.163%|

Screen conditions:
- at_least_one_batch_gain_over_half_percent: True
- no_batch_loss_over_half_percent: False
- no_warm_loss_over_half_percent: True
- no_single_loss_over_one_percent: True

Actual-use counts cover each whole process, not an isolated DB phase. All ON main and cache16 runs exceeded1,000 conditional external copies; all OFF counts were zero. No isolated percentage updates the complete matched XFS table.

Reversed confirmation completed24 main+4 cache16 runs and all28 full payload/revision fresh-process reopens with unchanged hashes, actual-use/size checks and physical cleanup. The short-batch losses violate the predefined -0.5% bound; REJECTED, not imported. Warm-read improvement and long-batch gains do not override that decision. No adoption helper, defaultON archive, or new full XFS pipeline was executed. All140 production C/H/inc files still match the adopted default50us source archive.


## Profile-guided hash-count store experiment

Read-only disassembly of the exact adopted50us binaries maps LevelDB background IP0x4730c8 to append `rep movsq` (33 raw instruction samples). RocksDB low IP0x9a9e35 is mapped-page `rep movsq` (109 raw samples), but its more frequent IP0x9a9e82 is `lock incl` updating `bm_hash_count[key % HASH_NUM]` (280 raw samples). High-thread counts at these two instructions are29/84. These are raw IP counts from the recorded exact TIDs, not elapsed-time shares or isolated causal costs. The first read-only objdump attempts used an option unsupported by the server's older binutils; address-range disassembly succeeded without any workload rerun or binary change. All diagnostic output is retained.

`hash-count-store-20261114` is a private default-OFF overlay on the exact adopted50us source; rejected read-copy/cadence/LRU overlays are absent. Only the increment in `nvfuse_write_mapped_cache` and `nvfuse_append_cached_page` changes from atomic read-modify-write to atomic read plus store while the existing `bm_lock` is held. Both helper paths acquire `bm_lock` before hash insertion and retain it through the increment. Other count updates, hash/LRU algorithms, references, all locks, layouts and durability paths remain unchanged. The same bucket count value is retained; no counter is removed. A separate whole64byte diagnostic structure records actual use at each site, with actual ELF isolation from runtime gate/timers required before timing.

Predeclared screen: both OFF/ON full v2/v3 validation, same-binary ABBAAB three trials per arm for both apps and100k/1M batch lengths, both-arm/application16MiB cache stress, all full payload/revision oracles/reopens and selected-controller restoration. At least one batch median gain must exceed0.5%; no batch/warm-read median loss may exceed0.5%, and no single-update median loss may exceed1%. OFF counts must be zero, ON counts must exceed1,000 per measured invocation and equal mapped+append site counts. All planned trials complete even if the performance screen fails early. Passing requires unchanged-binary reversed confirmation before adoption, then a new exact-default-source full XFS comparison; no isolated gain updates the XFS table. No production edit is part of this experiment.


The hash-count candidate built successfully and passed actual ELF64byte counter/gate/timer isolation, mapping unit validation and both OFF/ON full v2/v3 suites with validation cleanup. Its physical ABBAAB run is active; no production code or default has changed.


The hash-count store exploratory run was intentionally cancelled after the user requested cause-first analysis and raised elapsed-time concern:9 completed invocations and9 reopens, raw KeyboardInterrupt, unchanged hashes and physical cleanup PASS. Both-arm full v2/v3 had passed. This incomplete experiment has no adoption/performance-screen conclusion; partial logs remain on the server. A separate six-hour goal now focuses on the largest LevelDB short-batch gap.


## Six-hour LevelDB short-batch causal investigation

The focused deadline starts2026-11-14T14:36:30Z and ends20:36:30Z (Nov15 05:36:30 KST). The objective remains unachieved. No commits, pushes, production overlay adoption or unrelated cleanup occurred.

`level-causal-20261114` completed a private diagnostic build, full v2/v3 recovery/cache/mapping/fio checks, XFS smoke+3 full short-batch trials, NVFUSE smoke+same-binary ABBAAB3 trials/diagnostic arm, every full payload/revision fresh-process reopen, unchanged before/after hashes and selected-controller/hugepage restoration. XFS median164078.84ops/s versus NVFUSE diagnosticOFF144511.26 (-11.9257%); this narrow reproduction does not replace the earlier complete matched table. DiagnosticON median141027.20 is2.4109% below OFF, so diagnostic timings identify contributors but are not clean production throughput.

For1000 sync=true transactions updating100k rows, mean diagnostic background work is87351 writes/367.47MB and81432 loaded-cache reads/342.50MB. Background write core120.06ms, read core54.71ms, write gate wait62.98ms and read gate wait24.90ms. Foreground pwrite gate wait24.48ms/core27.14ms; fsync gate wait1.59ms/drain1.18ms/core55.88ms. Nested foreground fsync CQ wait39.55ms is INCLUDED in the core/drain interval and must not be added again. Different-thread intervals overlap. Outside foreground API time also includes input generation/DB work/backpressure and cannot be labeled entirely as waiting. Per-process task snapshots show XFS background CPU~525ms versus NVFUSE diagnosticOFF~663ms; this does not isolate equal amounts of compaction work by itself. Raw logs, exact workload, cumulative snapshots, validation results and analysis are retained under `build/test-results/leveldb-causal-20261114/`.

ELF inspection found the original `fs_gate` mutex spans40bytes at a64byte boundary, but its tail shares the same line with both `journal_interval` and the continuously polled `journal_deadline`. This is confirmed in both exact production and diagnostic binaries; its causal cost was not established by layout alone. `gate-isolation-20261114` is a private defaultOFF same-binary control selecting an identical mutex in a whole isolated64byte object. The original gate remains the OFF arm with actual ELF line-sharing assertions. All acquire/release/spin/journal/fsync/cache/data rules remain identical. Full v2/v3 candidate safety checks precede focused XFS3+NV ABBAAB3/arm with full oracles/reopens. A performance result is required before any adoption; the production source still matches the adopted default50us archive.


`gate-isolation-20261114` finished all3 XFS+6 NV full measurements/reopens, candidate full v2/v3 suites, actual runtime address/layout checks, unchanged hashes and physical cleanup. OFF144130.58 vs ON142388.38ops/s (-1.2088%); REJECTED, not imported. Timer/lock sharing is real but this intervention does not establish it as a positive causal performance contributor. All measured trials are preserved.

The next private candidate `append-init-20261114` targets the largest previously profiled component inside compaction writes: bounded new-page append initialization/copy. It preserves allocation, hashes/lists, locks, payload, zero suffix, publication and fsync rules. The OFF branch retains whole-page zeroing plus original copy; ON resets the same fields, copies payload once and zeros only the suffix, with conditional external libc memcpy for misaligned>=256byte source. There is no gate-isolation overlay. Actual-use counters occupy a verified whole64byte object. An exact96-shape source/destination guard, payload and zero-suffix oracle plus full v2/v3 recovery/cache/fio validation precedes focused same-binary ABBAAB3/arm and XFS3. Performance must be measured; no production adoption is implied.


The original append-init build stopped at the compile-time64byte counter-size assertion (alignment applied to all declarators); no device tests began. `append-init-v2-20261114` corrected the declaration in a new isolated stage. It passed exact96-shape source/destination/payload/zero-suffix guards, full v2/v3 validation,3XFS+6NV full measurements/reopens, actual-use/counter-layout and unchanged-hash/physical-cleanup checks. OFF140742.55 versus ON142523.19ops/s (+1.2652%), still -13.1912% versus this stage's XFS164180.55. Small initial benefit only; not adopted pending confirmation. No sum of isolated gains changes the full comparison table.

`env-causal-20261114` refines differential attribution before further candidate edits: a shared LevelDB Env wrapper delegates every selected call exactly once to either native XFS or the original NV Env and records originating-thread call count/requested/returned bytes and inclusive read/append/flush/sync/close/metadata time. The exact adopted production library/source/bridge and both DB libraries remain unchanged, hash-verified against the completed matched parity run. New app-only binaries use OFF/ON ABBAAB3 per arm on both backends, with all original workload/options/oracles and fresh reopens. Per-thread relaxed atomic loads/stores allow boundary snapshots without a data race or hot atomic RMW. Timed records are inclusive; different-thread time overlaps. This measures actual excess over XFS and compaction work amounts; it is not adoption evidence.


`env-sampled-20261114` completed common Env1/64 highfrequency timing with full call/byte counts, bothfs profileOFF/ON ABBAAB3/arm, all payload/revision reopens, unchanged production source/library/app hashes and physical cleanup. Timing estimates have sampling alias and measured overhead (NV profileON about -3.8%); do not interpret them as exact costs or production throughput. Timer-external DBstate observations do not drain/reset compaction and show the same state difference with profiling OFF: XFS enters short batch with files per level[1,3,27,1,0,0,0], NV[1,10,26,1,0,0,0] (ON9 at L1). NV L1 is about32MiB versus XFS7MiB. Thus batch background work is not equal: XFS roughly270MiB reads/324MiB writes versus NV359MiB/383MiB; higher total API time does not imply the same percall slowdown.

The preceding100k single-sync-write phase in the earlier causal snapshots shows foreground fsync core about1.98-2.00s, including nestedCQ1.45-1.47s. Background pwrite gate wait is about1.19-1.21s, cached-read gate wait0.67-0.68s, versus background writecore0.17-0.19s/readcore0.062s. Different-thread intervals overlap and cannot be added as wall time. This directs the next cause/fix toward fsync-held gate serialization and delayed compaction, rather than small cacheline/copy edits. `general-spin-20261114` first controls existing general gate spin10us vs50us with unchanged production binaries, identical app profileOFF/workload, full50usv2/v3 checks, XFS3+NVABBAAB3/arm, all original oracles/reopens and cleanup. Production remains unchanged; no result is claimed until terminal evidence.


`general-spin-20261114` completed configured50us fullv2/v3,3XFS+6NV full trials/reopens, unchanged hashes and physical cleanup. General gate10usOFF median144604.69 vs50usON139657.54ops/s (-3.4212%): REJECTED. Fullphase and DBstate evidence saved in `general-spin-analysis.json`; no production setting changed.

`fsync-read-20261114` is a new private defaultOFF concurrency candidate on the exact adopted archive (the4 edited base files individually byte-compared before overlay). The existing gate stays held across the entire typedfsync and all journal barriers. An atomic openbit+activecount admits only bounded loaded-cache reads (<=64KiB) under existingbm/bclock and ref/flush guards; readers never write descriptor offsets/size epochs or touch I/O/journal state. Typedfsync closes admission and drains all admitted readers before releasinggate. Generic callbacks/writes/lifecycle/AIO remain excluded. The new deterministic test stalls fsync to prove hotread completion and coldfallback; stalls an admitted reader while fsync exits to prove quiescence before truncate/unlink/fd reuse/pwrite/AIOallocation/stop. Fullv2/v3 recovery/cache/fault/fio follows before any physical samebinaryOFF/ON timing. DBworkload remains unchanged; no drain/reset, no production import.


`fsync-read-20261114` passed deterministic overlap/quiescence actions0-5 forv2/v3, full recovery/fault/cache/fio,3XFS+6NV trials and every full-oracle reopen. OFF143900.19 vsON134323.39ops/s (-6.6552%): REJECTED. ON admitted about20k cache reads/process but did not remove entering compaction debt; one entry attempt misses windows that open while the ordinary gate retry is already spinning. `fsync-read-v2-20261114` integrates admission into that existing bounded retry and repeats safety plus a newXFS/OFF/ON comparison.

External-state boundary: the unrelated dd-device soak runner timed out its pre-existing6-hour api-delete operation at2026-11-14T15:28:25Z; it had already created WORKER_STOP180s earlier and retained its active owner. Therefore the first candidate's broad cleanup flag isFALSE, preserved as-is. Separate original fields verify selected4ddevice driver+override/hugepages were restored, allNVMe drivers unchanged and SMART errors unchanged/zero. Read-only inspection found original fs_soakPID63381/start2525305667 still onddwith reactorcores1/3; no signal, driver, stopfile or payload modification was made. Follow-up comparisons explicitly require the exact failedstatus hash, unchanged worker identity/command andWORKER_STOPmtime before/after; allarms are measured afresh after this boundary. This is preservation of a failed external fixture, not soak success or recovery.


`fsync-read-v2-20261114` completed fullv2/v3+deterministic safety,3XFS+6NV full measurements and every reopen, unchangedhashes/physicalcleanup and fixed externalfixture PASS. OFF142878.22 vsON137575.16ops/s (-3.7116%), combined update1+batch time+1.5220%: REJECTED. ON enters withL0=4/L1=6 versusOFFL0=1/L1=10; debt moved towardL0 rather than resolving overallcompaction delay. Processwide actual concurrentreadhits~32-35k, zero misses. Read concurrency alone is insufficient; no production import.

`gate-pi-20261114` now isolates the largest remaining observed wait (backgroundpwrite~1.2s in the preceding100kfsync phase). It uses the exact original productioncache path, no concurrent-read overlay, same mutexobject/scope/acquire/release logic, but tests native PTHREAD_PRIO_NONE versusPTHREAD_PRIO_INHERIT to enforce kernelhandoff when blockedwaiters exist and prevent repeated ownerbarging. Protocolset/get/init must succeed; no silent fallback. Fullv2/v3+fresh3XFS/NVABBAAB3/arm precede assessment. Shortthroughput and combinedpreceding-single+batchtime must both be reported to avoid hiding a shiftedwait. Production unchanged.


`gate-pi-20261114` passed fullv2/v3,3XFS+6NV full measurements/reopens, unchangedhashes/physicalcleanup and preservedexternalfixture. NativePI handoff reduced shortthroughput by-5.5546% and increased combinedupdate1+batchtime by2.0185%: REJECTED, no production import. Nativefutex handoff overhead does not solve the application gap.

The next cause/fix targets actual gate-held I/O time (foreground100kfsync core~2s, nestedCQ~1.45s): the ordered-data write and subsequent metadata-log write are serial roundtrips. `small-log-20261114` privately logs complete data images only for boundedsmallflushes (<=8dirtypages, conservative remainingTXbudgetdirty+1, v3, standalone/noactiveAIO), in the existingv3 full-data record encoding alongside metadata. The entiregate, journalbarriers, replay and checkpoint remain unchanged. Large/vectored/v2/uncertainbudget paths retain orderedI/O. Earlier committed data overlays remain protected through existing lookup/epochreclamation rules. New public owner-only helpers restore the metadata-only TXpolicy after fullimage staging. All editedbasefiles individually matched the exact adopted archive.

MacClangO2/Werror independentpersistence oracle PASS before upload:120sectorpersist/torn/boundarycases,9000selective-data+metadata commits with actual pressure/epochreuse, budget/v2/inactive/alignment guards, TXpolicyrestoration, committedoverlayrestaging, fullcapacity/reopen cases. Server repeats these and originalengine/v3oracles before fullv2/v3; additional realprimaryfixtures require an unacknowledgedcross-page overwrite with failedlog to reopen oldbytes and thousands of WAL-likeappends to survive immediate_exit aftersuccessfulfsync, plus physicalblock/inodereuse. No production code/default adopted.


`small-log-20261114` completed original engine/v3 plus selective-data sector
persistence/budget/9000-commit oracles, full v2/v3, failed-overwrite rollback and
immediate-exit fsync append recovery, XFS3+NV6, every payload/revision reopen,
unchanged hashes and physical/external-fixture preservation. OFF143689.08 vs
ON143188.42ops/s (-0.3484%), XFS165140.69; single update+9.5165% and combined
update time-7.2707%. The target short batch still trails XFS13.2931%; no adoption.
Entering L1 count remains9-10. Reducing serial fsync I/O alone did not resolve
the short-batch debt.

`ticket-gate-20261114` tests a separate cause: native mutex reacquisition
barging can delay a blocked compaction writer despite adequate total CPU.
An optional FIFO ticket gate keeps every exclusive critical section, I/O,
cache, publication, fsync and journal barrier unchanged. It removes kernel
handoff syscall overhead measured by the failed PI candidate. OFF keeps the
native mutex, ON selects ticket acquisition/release consistently for caller,
cache, owner and lifecycle paths. A shared-header local oracle passed FIFO
nonbarging, 32-bit ticket wraparound, publication and160000 contended
critical sections. Full v2/v3 follows before XFS3+NV ABBAAB3 each and full
reopens. Production is unchanged; this is a causal candidate, not a gain.

## Focused investigation closeout, 2026-11-15 KST

**Target unmet; six-hour deadline missed.** The agreed time box was
2026-11-14 14:36:30–20:36:30 UTC (23:36:30–05:36:30 KST). At the final handoff
the deadline had already passed; closeout finished around 21:31 UTC / 06:31 KST.
No candidate from this focused investigation was imported, committed or pushed.
All 140 current C/H/inc files byte-match the adopted 50us source archive
`9728549999cde4cf5f5e62f22b030ef997764610b7bc7fb030b204409c506f75`.

### Largest supported contributor and its limits

The strongest measured explanation is **compaction lag accumulated in the
preceding single-sync-update phase**, followed by greater compaction work during
the short-batch timer. In profiling-OFF DB-state observations, XFS starts the
short batch at L0/L1 counts 1/3, while NVFUSE typically starts at 1/9–10.
Sampled Env diagnostics count approximately 270/324 MiB of background
reads/writes for XFS versus 359/383 MiB for NVFUSE during the batch. These are
different work amounts, so aggregate API time alone cannot establish a per-call
slowdown.

In the preceding 100k single-update phase, diagnostic background write/read
gate waits are about 1.2s/0.68s. Foreground fsync core time is about 2s, INCLUDING
1.45–1.47s of CQ wait. Fsync retains the exclusive gate during those I/O waits.
These cross-thread intervals overlap and must not be added. Diagnostic overhead
and sampling limit precision. The gate-held fsync path is therefore a strong
optimization target, but the exact fraction of the short-batch deficit caused
by it remains unproven: narrower read concurrency, different handoff policies
and small-write journal changes did not close the measured gap.

NVFUSE also completes the preceding single-update phase faster than XFS. In the
last complete candidate comparison, unmodified-behavior NVFUSE takes 3.521093s
for single+batch updates versus XFS 3.703385s. This does not satisfy the selected
short-batch target, but prevents interpreting phase-local compaction debt as a
regression of the whole update sequence. No compaction drain, inserted sleep,
phase-boundary reset, workload change or relaxed sync was used to hide the debt.

### Final completed candidate results

`ticket-gate-measure-20261114` completed all three XFS and six NVFUSE trials and
fresh-process reopens, unchanged hashes and cleanup. Ticket handoff changes
short throughput 143584.97→143430.43ops/s (-0.1076%), with combined update time
-3.0702%. Short throughput still trails its XFS control by 12.7999%.
Not adopted. Earlier build/count-threshold failures and the corrected immutable
binary measurement are retained in `ticket-gate-summary.json` and raw evidence.

`payload-copy-v2-20261114` completed full v2/v3 validation, all three XFS and six
NVFUSE measurements, payload/revision oracles, fresh-process reopens, unchanged
hashes and cleanup. Median of three trials per arm, ops/s:

| Phase | XFS | NVFUSE original behavior OFF | Copy candidate ON | ON vs OFF |
| --- | ---: | ---: | ---: | ---: |
| Warm read | 180532.80 | 178902.44 | 189141.13 | +5.72% |
| Single sync update | 32302.01 | 35182.59 | 35124.33 | -0.17% |
| Short batch100 / 100k updates | 164581.47 | 147323.56 | 144286.91 | -2.06% |

The OFF short-batch control is -10.49% versus XFS; ON is -12.33%. These are
candidate-stage controls, not a refreshed complete production XFS comparison.
Do not subtract them from the historical -12.75% to claim an adopted gain.
The candidate is rejected. Different-thread timings and median-of-three results
do not establish confidence intervals.

### Last candidate cancellation and preserved evidence

`sync-batch-20261114` privately extended the existing small-data journal
experiment from 8 to 48 dirty pages and separated 128KiB versus 256KiB raw
transfer caps. The hypothesis was that a roughly 100KiB batch misses the
8-page route and retains serial ordered-data plus metadata I/O. This is still
an **untested performance hypothesis**, not an established cause or fix.

Its build, original engine/v3 oracles, selective-data 9000-commit/capacity
oracles, and full v2/v3 integration checks passed, including 32-page failed-log
overwrite rollback and successful-fsync overwrite/append immediate-exit reopen.
The subsequent physical runner had already entered its XFS measurements when
closeout interrupted it. The raw result is intentionally `status=fail` with
`KeyboardInterrupt`; no complete XFS/NVFUSE comparison exists and no performance
conclusion is drawn. The outer orchestrator was terminated to prevent further
stage launches; the physical runner executed its existing cleanup path.

Final cleanup PASS: benchmark lock free, selected `0000:4d:00.0` restored to
`nvme`, override `(null)`, benchmark mounts absent, node0 hugepages restored to
1024. The runner records no cleanup errors, unchanged artifact hashes, unchanged
SMART media errors and preservation of the pre-existing failed independent
`dd`-device soak fixture. That separate soak is not claimed healthy or recovered.

Local evidence: `build/test-results/leveldb-causal-20261114/`, including
`payload-copy-v2-20261114-analysis.json` and
`remote-evidence/sync-batch-20261114/deadline-closeout.json`. Remote raw evidence
remains under `/home1/irteamsu/nvfuse-dev/run/`. The next useful experiment, if
work is resumed, is the prepared four-arm batch-size/transfer-cap comparison;
it requires actual completed performance evidence before any adoption.

### Post-closeout source clarification, no new measurement

Read-only inspection narrows the serialization mechanism further. With the
primary journal enabled, `nvfuse_fsync(fd)` validates the descriptor and calls
the global `nvfuse_primary_journal_commit`; the descriptor does not select the
flush set. The commit forces the buffer manager's entire dirty list through
`nvfuse_check_flush_dirty` / `nvfuse_flush_dirty_data`. Typed `nvfio_fsync`
holds `fs_gate` across buffered-writeback drain and that full commit. Therefore
a foreground WAL sync can also flush dirty SST pages from background compaction
while excluding further background cache operations. This mechanism is
confirmed by the source, but its incremental cost versus required journal
ordering has not been isolated. Simply skipping other files' dirty pages could
violate the existing transaction's ordered-data requirements and is not a
validated fix.

An additional measurement requirement for the prepared journal candidate is
phase-specific actual-use evidence: its current process-wide promoted-page and
commit counts can be satisfied by the 100k preceding single updates alone.
They cannot prove that the short-batch phase used the 48-page route. A resumed
comparison should capture counter deltas outside the short-batch timer, and
report its gate-held I/O time and total preceding-single-plus-batch time.

## User-resumed investigation, 2026-11-15 KST

The user explicitly reactivated the goal after the time-box closeout. Work
resumed at 21:54:32 UTC; the original missed deadline remains recorded.

`sync-batch-phase-20261114` completed full engine/v3/selective-data oracles,
full v2/v3 integration, three XFS plus nine NVFUSE measurements, every fresh
payload/revision reopen and unchanged-hash/selected-device cleanup. A private
gate-only counter accessor reads existing counters outside phase timers;
it does not drain writeback, change epochs or issue filesystem operations.
All three NVFUSE arms share the exact binary and workload.

| Condition | Short ops/s | Change vs ordered OFF | Single-update ops/s | Single+batch seconds |
| --- | ---: | ---: | ---: | ---: |
| XFS | 164792.28 | — | 32447.71 | 3.689174 |
| Ordered-data OFF | 144425.50 | — | 35122.94 | 3.539541 |
| Selective data log, <=8 pages | 143725.85 | -0.4844% | 39089.83 | 3.253979 |
| Selective data log, <=48 pages | 127119.24 | -11.9828% | 40513.48 | 3.254977 |

Phase deltas establish actual short-batch route use: the 8-page arm promotes
a median 68 pages in 62 commits; the 48-page arm promotes 22515 pages in
790 commits. Expanded coverage therefore works, but short-phase journal bytes
increase from 6434816 to 142897152 and home bytes from zero to 98107392
(medians of counters, not additive medians of total physical traffic).
The larger data-log policy adds journal/reclamation traffic and loses short
throughput. REJECTED for this target; neither policy is adopted. Its faster
single-update phase and combined sequence do not fulfill short-batch parity.

The proposed 256KiB raw-transfer arm was omitted after source inspection:
`nvfuse_journal.c:run_limit` already limits contiguous engine transfers to
32 pages. Raising only the transport cap cannot produce a 256KiB journal I/O.
The three measured arms retain the same 128KiB cap. The counter accessor and
all selective-data source are private, outside production.

The next private candidate, `stream-wb`, keeps the existing 256KiB per-file
stream qualification and all cache/DMA exclusion, journal and fsync barriers.
After qualification, it checks for completed pages every 4KiB with a one-page
minimum, instead of every 64KiB with a 16-page minimum. This tests whether
starting ordinary data I/O before foreground fsync reduces gate-held waiting
without data-log write amplification. It includes phase-only writeback counters
and deterministic single-page paused-DMA exclusion/recovery checks. It is
distinct from the previously rejected `early-wb-20261114`, which lowered only
stream qualification to 64KiB while retaining the 16-page minimum.

`stream-wb-20261114` completed PASS with all new single-page DMA tests, full
v2/v3 integration, XFS3 + NVFUSE6 timed trials, every fresh-process
payload/revision reopen, immutable hashes and device/hugepage cleanup. OFF
143114.50 versus ON142353.06 short ops/s (-0.5320%); XFS164508.00 (-13.4674%
ON versus XFS). Single-update OFF35003.90 versus ON32334.51 (-7.6260%); combined
update time increases6.7635%. REJECTED, no production import. ON moves roughly
87-89k short-phase async pages in39-40k batches versus OFF82-85k in4.3-4.6k
batches, while forced dirty pages fall from44-46k to38k. The extra submission
frequency costs more than it saves; L1 entering debt remains9-10. Pending-page
snapshots are instantaneous state, not cumulative counters. Evidence archive
SHA2562258b85e248f52ed34597c308982e0cd854f4829d2c29866edc7601da15633cb
is verified locally under `leveldb-causal-20261114/remote-evidence/`.

`dirty-order-20261114` tests the current forced-data path's submission
fragmentation: the latest OFF short phase has median45580 dirty pages and29926
requests, because the existing merger sees adjacent positions in dirty-list
order only. A private submission view groups protected requests in their
original relative order, then sorts ordinary data by physical address and
merges within the existing vector limit. Every original job remains allocated
and every page remains FLUSHING until all existing completion waits finish.
Overlapping ordinary data offsets fall back before any mutation; protected
images never merge. Original writeback/gate/journal/flush rules remain intact.
This is a hypothesis about request fragmentation, not a claim that it explains
the entire deficit. Full v2/v3 tests precede same-binary ABBAAB3/arm and XFS3;
phase counters must demonstrate actual request reduction alongside throughput.

`dirty-order-20261114` completed full v2/v3, all XFS3+NV6 measurements and
reopens, unchanged hashes and selected-device cleanup PASS. OFF141604.64 versus
ON143601.46 short ops/s (+1.4101%); XFS164759.28, still -12.8417%. Single
OFF35112.95 versus ON34706.73 (-1.1569%); combined updates3.546845 versus
3.577658 seconds (+0.8687%). This is a small initial result, not parity or
adoption. The source and validated binaries remain private. Immutable reversed
BAABBA confirmation was launched only after terminal PASS+cleanup and rechecking
every original artifact hash; it inherits the exact-source v2/v3 validation
without rebuilding or modifying any source/library.

`dirty-order-confirm-20261114` completed XFS3+reversed NV6 and all fresh
reopens, immutable hashes, external-fixture preservation and cleanup PASS.
OFF143959.36 versus ON141720.40 short ops/s (-1.5553%); combined update time
+0.9549%. XFS164188.06, ON still -13.6841%. The first +1.4101% did not repeat,
while the single/combined regression repeated. REJECTED, no production import.
Reducing request fragmentation does not establish a useful end-to-end fix here.

`prepared-pages-20261114` now tests work performed under the gate rather than
changing handoff or I/O ordering. Diagnostic background pwrite core is about
120ms/87k calls/367MB during short updates; saved exact-TID append IP samples
identify payload copying. Prior memcpy-dispatch tweaks did not close the gap.
For typed2048..8192byte writes, an optional16-slot workspace prepares at most
three zero/gap-preserving DMA pages before acquiring fs_gate. Existing helper
checks, inode/bm/bc locks, unreferenced/no-BH/no-flush/no-DMA state or exact
new-page allocation then permit an owned payload-pointer exchange. Existing
pages copy their unwritten prefix/suffix while locked; new pages retain zero
gaps. Every page has exactly one cache/workspace owner. Pending DMA, live AIO
owner fallback and uncertain cases keep original copy behavior. No cache page
gets exposed outside the gate; all filesystem state and fsync boundaries stay
serialized. The fixed196608byte transient DMA workspace is extra memory,
separate from the512MiB read cache, and must be reported in comparisons.

The exact shared helper passed32768 all-offset/size/fresh/overwrite guarded-byte
and unique-ownership cases with local Clang ASan/UBSan. Server repeats that
oracle and fullv2/v3 before any XFS3+NV6 physical measurement, with actual
exchange counts, all original phase/oracle/reopen and cleanup checks. Source
archive is the exact original adopted baseline, without sort/data-log/early-WB
or concurrent-read overlays. This remains a private unmeasured hypothesis.

The first prepared-page build and exact helper oracles passed. Validation
commands through v2 cache-create/check passed functionally, but the newly added
actual-use assertion correctly stopped the orchestration: swaps were zero.
The old fixture used1byte/32KiB typed writes or generic callbacks, so it did not
exercise the2048..8192byte candidate. No physical performance run began;
validation cleanup passed. This failure and original logs remain intact.

A distinct `prepared-pages-v2` retains the exchange algorithm and adds an
explicit4101byte append/8192byte three-page RMW fixture, full-byte/EOF fresh
reopens and immediate_exit after fsync. Existing paused-DMA tests now write
4KiB typed chunks under ON, and their overlapping typed overwrite is4KiB;
faulted appended-data writes are4KiB so recovery covers exchanged pages. The
fixed workspace's actual shared claim/release helper also passed exhaustion,
fallback, publication and40000 concurrent claims under local ASan/UBSan and a
separate TSan run. The gate-only boundary accessor now snapshots cumulative
prepared fresh/existing/byte counters, requiring actual short-phase use.
Logging newlines are corrected. No source, test or binary from the original
failed stage is overwritten. The v2 extra workspace remains196608bytes.

V2 candidate local TSan also passed the exact shared pool publication/ownership
oracle. Real-primary v2 and v3 cache-create each report534 fresh and156
existing exchanges over2204348bytes, proving actual use in the new fixture.
The candidate's v2 immediate-exit/repeated-reopen, prepared-page buffered-DMA
exclusion and failed-log original-prefix/EOF recovery tests all passed.
Production140 C/H/inc files were rechecked against the adopted archive and
remain identical; all new code and helper tests are private experiment files.

`prepared-pages-v2-20261114` completed all v2/v3 checks, XFS3+NV6,
phase-specific actual-use proofs, every reopen, unchanged hashes and device
cleanup PASS. Short OFF142033.62 versus ON141867.74ops/s (-0.1168%);
XFS164313.44. Combined single+short time3.556278 versus3.535971s (-0.5710%).
The short target failed; REJECTED, no production import. Local raw evidence
and `prepared-pages-v2-20261114-analysis.json` preserve all trials.

A new private `commit-cache-20261114` targets the earlier measured preceding-
phase background gate waits (~1.2s writes/~0.68s reads). Only a typed standalone
v3 fsync may admit a single bounded cache executor AFTER dirty-data collection
and superblock snapshot, while the owner commits independent journal images.
The owner retains fs_gate, closes admission and waits for its one admitted
reader/writer before restarting the transaction or touching cache again.
Loaded reads <=64KiB and strict EOF appends <=8KiB retain cache/inode locks,
references and allocation accounting; cold/pinned/new-pointer-path/unknown-
classification/sync/direct cases fall back. Writes invoke no I/O, journal
mutation, dirty eviction or generic callbacks. Their dirty data/metadata belong
to the NEXT transaction; no snapshot or successful-fsync barrier is relaxed.
This concurrency policy is experimental and not imported.

The atomic single-executor helper passed local Clang ASan/UBSan and TSan
publication/exclusion tests. Server build and original engine/v3 oracles passed.
The first AIO runner stopped on a leftover previous-candidate log assertion.
A separate validation-v2 reached the real overlap test; action0 passed, but a
strict hot-append expectation on a reused/truncated file entered its legitimate
ordinary fallback while the test deliberately held the fsync owner. Its timeout
and raw failed cleanup result are preserved. The kernel hugepage shrink
completed after the immediate final read; a separate cleanup recheck confirmed
1024 pages and no surviving test process, without editing the raw report.

A separate test executable uses fresh file identities and a nonblocking
actual-helper probe. Production implementation/library/DB binaries were not
rebuilt or changed. `validation-v3` passed all v2/v3 recovery, cache, DMA/fault,
fio CRC and six-profile smoke checks, including actual reader/writer progress,
quiescence before truncate/unlink/pwrite/AIO/stop, crash/repeated-reopen of the
old immutable snapshot and of the subsequently acknowledged next transaction.
Its hugepage cleanup passed. No physical performance result exists yet.

Automatic approval review rejected the physical launcher because it reformats
the entire7.68TB selected NVMe; device identity/inactivity alone did not establish
erasure authorization. The rejected launcher was not run. Only temporary1GiB
file-backed AIO validation followed. Read-only inspection reverified selected
BDF/serial/capacity, driver nvme/override(null), no mount/holders/users and the
last candidate's cleanup. Explicit user approval for the selected-device
XFS/NVFUSE reformat comparison is pending; no permission is inferred from time.
Prepared reviewable runner: `build/test-results/leveldb-causal-20261114/commit-cache-physical-v2.py`.

Additional `extra-validation` PASS+cleanup: actual original helper handles
8KiB three-page and established indirect-path appends; a new pointer-path
boundary copies only its9byte cached prefix before ordinary fallback, and an
evicted partial EOF page falls back without issuing I/O inside admission.
Readonly/O_SYNC/O_DIRECT aliases, oversize and non-EOF calls are rejected by the
cache-only helper. Exact full bytes and EOF pass after serialized continuation
and fsync. A forced log failure while a concurrent next-transaction append
succeeds reopens the previously acknowledged prefix/EOF twice, unchanged.
The original implementation/library/DB artifact hashes remain immutable.
Final validation archive is saved locally as
`build/test-results/leveldb-causal-20261114/commit-cache-validation-complete.tar.gz`.
All140 original production C/H/inc files still match the adopted archive.
The physical runner now requires a specific explicit user-approval record and
cannot execute the formatter without that record. Approval remains pending.

Read-only continuation audit at 2026-11-14T23:05:22Z: all nine entries in the
server's validated build-proof still match. validation-v3 has49 passing commands
and extra-validation has5; both retain cleanup PASS and1024 hugepages before/after.
The physical result and explicit approval record are both absent. Selected
0000:4d:00.0 remains kernel nvme/override(null), live serial BIDCT0002I040CJ1H,
capacity7681501126656, currently /dev/nvme23n1; no mount, child device, holder or
fuser user was observed. No formatting or physical benchmark was launched.

Recomputed three diagnostic trials of the preceding single-update phase give
median background pwrite gate wait1.205762184s, cached-read wait0.677860957s,
foreground fsync core1.989792420s including CQ wait1.465467756s. These intervals
overlap and cannot be added. Gate serialization plus compaction debt remains
the leading measured mechanism, not a quantified explanation of the full gap.
The last complete comparison has adopted OFF142033.62 versus XFS164313.44ops/s
(-13.5593%); the rejected prepared-pages ON141867.74 is -0.1168% versus OFF.
No target completion or production adoption is claimed. The original six-hour
deadline was missed; the recorded continuation deadline is not reset.

Blocked audit at2026-11-14T23:06:33Z: third consecutive goal turn with the same
explicit selected-device erasure-authorization blocker. Live read-only server
recheck again finds no approval record and no physical result; all9 validated
artifacts match, both validation reports retain PASS+cleanup, and the selected
device remains unmounted kernel nvme with no holders/fuser user. Goal is blocked,
not achieved. The authoritative goal deadline remains2026-11-14T20:36:30Z;
historical continuation-deadline metadata is not a user-authorized extension.
No meaningful remaining independent check can establish the required physical
performance result. Await explicit user authorization before selected-device
erasure; preserve the current code, evidence, and unrelated failed soak fixture.

### Explicitly approved commit-cache physical comparison

The user explicitly approved selected-device formatting and repeated comparison
at2026-11-14T23:08:57Z: "해당 장치 포맷과 반복 측정 승인". The approval was
recorded with the selected BDF/serial/capacity before execution. Rechecked
identity/inactivity and immutable validated artifacts, then ran the prepared
physical-v2 runner (PID17114/start2530251896). It terminated PASS with all
fresh-process oracles/reopens, immutable hashes and cleanup PASS. Original
six-hour deadline failure is retained; no retroactive completion is claimed.

| Three-trial median | XFS | NVFUSE OFF | NVFUSE ON |
|---|---:|---:|---:|
| Short batch100/100k ops/s |164667.33|143831.75|143494.15|
| Preceding single-update ops/s |32399.04|35264.33|36179.56|
| Single+short seconds |3.693796|3.532522|3.460162|

ON/OFF short=-0.2347%; OFF/XFS=-12.6531%; ON/XFS=-12.8582%.
ON single=+2.5953%, combined update time=-2.0484%. Short target failed;
candidate REJECTED for this objective and remains unimported. No reverse
confirmation or application regression suite is justified for adopting it.
Raw short samples are XFS[165332.52,164667.33,164073.57],
OFF[146179.92,143831.75,143514.26], ON[143494.15,140886.48,148886.03].

Actual ON short-phase concurrent reads604–741, writes330–385 and payload
1.37–1.57MB prove the path ran. Batch-begin L1 counts decline from10 in all
OFF trials to6/7/7 ON, versus3 XFS. This intervention changes debt but does not
improve short throughput: the prior debt hypothesis is insufficient as the
standalone explanation. It does not prove debt has zero contribution. Next
diagnosis uses existing sampled Env counters on the same immutable binary to
compare actual work volumes and API costs; no compaction drain/policy change.

Selected device returned to kernel nvme/override(null), no mount; hugepages
1024→1024; unrelated driver changes{}. The independent failed dd-device soak
fixture remains unchanged. Archive `commit-cache-physical-complete.tar.gz`
SHA25659f64b7bfbae44de3a8fb0bf5cdf4b45d0cac199fba7bc699189865c0949be1b;
analysis `build/test-results/leveldb-causal-20261114/commit-cache-20261114-analysis.json`.

### Same-binary work-volume diagnosis after commit-cache intervention

`commit-cache-env-20261115` completed PASS+cleanup with the exact nine validated
artifacts, inherited explicit device-format approval and only the existing
common Env 1/64 sampled timing/counts enabled. XFS3 and NV ABBAAB3/arm, all
payload/revision reopens and immutable hashes passed. No implementation rebuild.
This is diagnostic throughput, not a replacement for the unprofiled table.
Atomic boundary snapshots are outside timers and bracket some background work;
sample extrapolation/aliasing and instrumentation overhead limit timing precision.

| Diagnostic median | XFS | CC OFF | CC ON |
|---|---:|---:|---:|
| Preceding single BG read MiB |530.476|334.682|469.899|
| Preceding single BG append MiB |558.435|384.245|508.721|
| Preceding single BG Flush estimated seconds |0.262|1.477|1.407|
| Preceding single BG Read estimated seconds |0.115|0.789|0.385|
| Short BG read MiB |258.615|346.162|333.306|
| Short BG append MiB |309.236|368.383|365.453|
| Short BG Flush count |76990|91728|90986|
| Short BG Flush estimated ms |143.507|184.566|212.055|
| Short BG Read estimated ms |57.264|110.363|98.062|

Different-thread intervals overlap; do not sum them as wall time. Env Flush
includes the underlying buffered pwrite and gate waits; Append bytes are
logical Env bytes, not device traffic. Common main-thread short counts are
8358 Append,4179 Flush and1000 Sync, with the same99.603MiB payload.
The intervention materially increases preceding compaction progress, but short
work volumes shrink only modestly and remain above XFS. Lower L1 file counts
are therefore an inadequate proxy for how much compaction work the batch faces.

The still-large preceding BG Flush interval motivates one configuration-only
control: enable the same immutable-commit cache executor in BOTH NV arms and
compare existing general-gate retry10us versus50us. `commit_cache_write_gate`
falls back to the ordinary mutex after that budget, so a sleeping writer cannot
participate in later open commit windows. Unlike the earlier rejected generic
spin experiment, retries here can complete actual cache work. No safety, sync,
format, workload or compaction-policy change; profile OFF. Require printed
actual budgets and phase-specific cache-use counts, fresh XFS reference,
combined-update time and reversed confirmation before any adoption.

Diagnostic archive SHA256ba00ae6150d912316ea23e65414583b6e31ab041d45361f662c3867a53ccd8c6;
analysis `build/test-results/leveldb-causal-20261114/commit-cache-env-20261115-analysis.json`.

### Commit-cache enabled, existing general-gate retry10us versus50us

`commit-cache-spin-20261115` completed PASS+cleanup, immutable hashes, every
reopen, phase use and actual printed10/50us budget checks. Both NV arms have
commit-cache enabled; labels0/1 represent retry budgets, not cache OFF/ON.
Profile remains OFF. This is an unadopted initial screen.

| Median of3 | XFS | CC+10us | CC+50us |
|---|---:|---:|---:|
| Short ops/s |164638.09|148368.27|157321.23|
| Single ops/s |32462.96|36175.11|38023.18|
| Single+short seconds |3.686896|3.438331|3.266145|

50/10 short+6.0343%, single+5.1087%, combined time-5.0078%.
50/XFS short=-4.4442%: target remains unmet. Raw50us short samples
[157321.23,147477.98,162459.00] show material variability; do not declare
reproducible gain or adoption from this screen alone. Short concurrent write
counts rise from331–419 to1615–1774, with actual copied bytes1.37–1.72MB
versus6.65–7.32MB. L1 entering files7→5, XFS3. No synthetic prebatch delay,
drain, compaction-policy change or relaxed sync. A narrower50/100us comparison
uses the same immutable binary next; a passing configuration still requires
reverse-order and broader regressions before source adoption.

Archive SHA256a9ab1ad1a8ab9c072fa197a8536756e5dbce95663f0cd7dfaad31926503a084b;
analysis `build/test-results/leveldb-causal-20261114/commit-cache-spin-20261115-analysis.json`.

### Commit-cache enabled, retry50us versus100us

`commit-cache-spin100-20261115` PASS, cleanup PASS, immutable hashes and all
payload/revision reopens passed. Both arms enable commit-cache; profile OFF.

| Median of3 | XFS | CC+50us | CC+100us |
|---|---:|---:|---:|
| Short ops/s |164674.38|161026.01|157039.68|
| Single ops/s |32423.62|38344.29|38220.24|
| Single+short seconds |3.691431|3.222018|3.261016|

100/50 short -2.4756%, combined time +1.2104%. 50us/XFS -2.2155%,
versus -4.4442% in the previous series: the deficit persists and variability
requires reverse confirmation. Reject100us; do not continue expanding retry
budgets. Neither configuration is imported into production. Archive SHA256
b8720424a1365cf041d41df22258620a6b8c851452337b69acfd32e636463c42.

Next causal test uses CC ON and50us fixed: advisory gate hint OFF/ON. Read-only
busy checks can skip failed repeated trylock operations, but every actual
entry uses the original pthread mutex. First try and bounded fallback are
unconditional; clear hint before unlock; stale hints affect performance only.
All runtime acquire/release sites, including buffered writeback and commit
cache retries, use the same shared helper. Default OFF. Skip counts accumulate
locally per wait, then atomic-add once on return, including concurrent-cache
completion. No per-probe atomic RMW. Actual shared helper passed local
Clang ASan/UBSan and separate TSan with180000 operations per OFF/ON arm, plus
forced stale-hint checks. Private source only, not a performance conclusion.

Prior-work control: `gate-ttas-20261114` had already rejected a general-gate-only
read-mostly hint (LevelDB long -0.28%, RocksDB +0.04%). It used a separate hint
cacheline,10us general spin, and predates caller-cache retry and commit-cache
admission. The present experiment specifically covers cache-read/write retry
loops with CC ON+50us and co-locates the hint with the mutex. It does not erase
that negative evidence or establish a new bottleneck from the older profiles.
The incremental effect must come from this new OFF/ON measurement.

### Advisory gate hint: rejected after same-binary comparison

`gate-hint-20261115` passed full54 integration/extra validation commands, unit
oracles, all physical payload/revision/reopen checks, immutable artifacts and
cleanup. Both NV arms CC ON+50us. Median3 short ops/s: XFS163997.47,
hintOFF159501.70, hintON159588.24. ON/OFF+0.0543%, ON/XFS-2.6886%; no
material gain. Single medians32434.42/38689.49/38816.03; combined update
seconds3.692909/3.211634/3.202868. ON skipped36.2M/38.0M/39.0M retries, so
the actual-use condition passed but fewer RMW calls did not improve the target.
Reject and do not import. This cost is not demonstrated as the dominant
remaining bottleneck. New baseline CC+50us still trails XFS by2.7414%.

Archive10042a1cf0be48ac415f54a2f9897ca405a1640cc3d6a288fedd7c2f8f01de61;
local `gate-hint-20261115-analysis.json`. Selected nvme/override(null), unmounted,
hugepages restored1024; unrelated driver changes{}, failed dd fixture unchanged.

### Inode size-validity investigation

Source observation: typed pwrite and typed fsync call global `changed_epoch`,
so even unrelated WAL/SST writes invalidate the size cache for every reader.
A background cached read can repeatedly look up and promote its inode-table
page despite reading an unchanged SST. The older pre-CC exact-TID read self
profile suggests read overhead, but does not quantify this specific cost.
Treat it as a hypothesis, not an established dominant bottleneck.

Private `size-generation-20261115` tests global invalidation OFF versus hashed
per-inode volatile generations ON, both CC ON+50us and gate hint absent. Typed
writes invalidate all aliases of that inode; generic callbacks/truncate/open/
close/lifecycle still invalidate globally. Journal fsync preserves size stamps
because it does not alter file sizes, while any admitted CC append invalidates
its own inode. Legacy nonjournal sync stays conservative.256 aligned buckets
allow false invalidations on collision, never false validity; generation wrap
invalidates globally, global wrap clears all stamps. No on-disk/layout change.

Shared helper passed ASan/UBSan alias,collision,identity,wrap checks and200000
actions against independent revision state. New actual-runtime tests require
zero size rechecks after unrelated typed write+fsync, correct EOF/bytes across
three aliases after typed append, generic truncate and gap creation. Existing
CC shape oracle now also reads its read-only alias after concurrent append.
KVSIZE boundary counts are read under existing mutex without drain/sleep and
outside phase timers. Same extra counters exist in both arms. Performance
and actual recheck reduction remain unmeasured until terminal physical results.

The new v3 alias fixture found a pre-existing hole-content failure: generic
truncate4096->128, then typed write at2048, reads byte65 at128 instead of zero.
The initial `validation-v3` failed and is preserved. Its returning CHECK also
left the SPDK test alive; only that verified test process group31938 was killed.
The immediate cleanup snapshot recorded2048 pages versus1024 before; after
process reap the requested pool shrink completed and live count returned1024.
The original cleanup=false remains, with separate post-reap-cleanup evidence.

A fresh-image v2 diagnostic reproduces the identical first_bad128/actual65/expected0
with candidateOFF andON. This is a baseline data-semantics bug, not a newly
introduced size-stamp regression, and is NOT marked fixed or validated. Both
modes still report correct new EOF; all alias checks before the gap passed.
The same diagnostic reports unrelated-write+fsync size rechecksOFF3 versusON0,
confirming the proposed invalidation mechanism in a controlled fixture only.

`validation-v4` preserves that failure and uses a distinct alias fixture with
contiguous append after truncate, so it can validate this optimization's actual
size/alias invariants without treating baseline hole semantics as passed. The
full existing v2/v3/CC/fault/CRC suite reruns from fresh images. Runtime/library
hashes are unchanged. No physical performance measurement had started when
the failure occurred. Hole-content correctness remains a documented limitation.

### Inode size generations: rejected after measured count reduction

`size-generation-20261115` v4 completed51 integration+5 extra commands, plus
all physical payload/revision oracles/reopens, immutable hashes and cleanup.
The earlier hole-content failure remains recorded separately and is not fixed.
Both measured NV arms CC ON+50us; no gate hint. Same-binary median3:

| Metric | XFS | Size-bucket OFF (global invalidation) | Inode generations ON |
|---|---:|---:|---:|
| Short ops/s |164435.13|159172.93|155128.25|
| Single ops/s |32426.95|38214.40|38205.88|
| Single+short seconds |3.689986|3.243740|3.262026|

ON/OFF short-2.5411%, combined time+0.5637%; ON/XFS-5.6599%. Reject,
no import and no reverse stage. OFF/XFS=-3.2002% is the latest matched private
CC+50us control; the source adoption status does not change. Raw short OFF
[159172.93,145654.05,161799.45], ON[155128.25,147865.36,164404.25].
Do not select the final near-parity ON sample as the result.

Short inode-size recheck medians OFF74400 versusON945; loaded read calls
OFF74502 versusON74963. The extra lookup was real and almost eliminated, but
it was not the dominant demonstrated throughput bottleneck. Short read work
varied roughly74k–80k calls with compaction timing; medians of3 are coarse.
No sleep/drain, compaction-policy change or sync relaxation was introduced.

Archive SHA25634906a79e7022a1d512830f9fbf85d30c897eba7389e20cda1011af8ebe99512;
local `size-generation-20261115-analysis.json`. Device restored to nvme, override
(null), unmounted, hugepages1024, unrelated driver changes{}, failed dd fixture
unchanged, SMART unchanged. All140 current production C/header/inc files again
match the adopted baseline archive; both new candidates stayed private.

The original20:36:30UTC six-hour deadline was missed. No explicit timebox reset
has been granted. Repeated formatting authorization resolved the prior device
blocker and enabled these completed comparisons; it is not a claim that the
performance objective was achieved. No automatic next stage is queued. The
remaining demonstrated mechanism is global fsync exclusion with additional
background compaction work; the exact incremental share of the short deficit
remains unproven. Further blind trylock/size-cache tuning is not justified by
these results. Any next investigation should refresh per-phase ordered-data
versus journal-completion time and actual BG work on the best configuration.

Final live read-only audit at2026-11-14T23:56:56Z passed: selected serial and
capacity verified, /dev/nvme23n1 unmounted/no users/holders, kernel nvme and
override(null), node0 hugepages1024, all measured hashes unchanged, both stage
processes gone, lock available and unrelated failed dd fixture preserved.
Local `selected-candidate-closeout.json` SHA256
0f709342d9b4798e29b67bfeeaf5cfaa05e589180a0028061b945b6a1337d661.
Objective unmet, original deadline missed; neither experiment was imported.


## File-scoped fsync, new user-authorized12h timebox (2026-11-15)

The user authorized a new12h period starting00:13:24UTC /09:13:24KST,
ending12:13:24UTC /21:13:24KST. The original6h deadline remains missed.
Progress snapshots are written every30min to `/private/tmp/nvfuse-fsync-progress.md`.

Experimental `NVFUSE_JOURNAL_FILE_SYNC=1` adds a target-only data collector and
private committed-baseline inode/indirect/allocation metadata images. Shared
live cache/accounting stays unchanged; unsupported creation/free/reuse/truncate
or already-staged-overlay dependencies use the full commit. See
[file-scoped fsync design](file-scoped-fsync-design.md). Default remains OFF.
This uses the adopted baseline and excludes all prior commit-cache overlays.

Initial real-SPDK AIO-image validation passed15 scenarios /75 commands, including
immediate ACK crashes, repeated original-image recovery, exact free counts,
direct/single/double-indirect boundaries, two-file successive commits and
conservative fallbacks. All15 retained images passed independent journal replay
and bitmap/reachability/accounting checks. Existing v2/v3 primary crash/replay,
long partial append, cache/AIO/writeback/fault regressions and final independent
checks also passed. Native generic journal/transaction persistence suites passed.
The first independent-tool sanitizer link failed because this server lacks GCC
ASan/UBSan shared runtimes; it is preserved under `validation-full`. The corrected
`validation-full-v2` ran the independent tools without sanitizers and passed.

First focused physical screen: fresh XFS3, same-binary NV OFF/ON ABBAAB3/arm,
profile OFF, same WAL/sync/CPU/cache/workload conditions, complete payload/revision
oracles and every fresh-process reopen passed. No artificial prebatch drain,
sleep or compaction-policy change. Median of3:

| Metric | XFS | File scope OFF | File scope ON | ON/OFF |
|---|---:|---:|---:|---:|
| LevelDB batch100/100k ops/s |160248.70|142046.51|144918.06|+2.0216%|
| Single sync ops/s |32280.29|34948.25|37468.61|+7.2117%|
| Single+short seconds |3.721895|3.565368|3.357422|-5.8324%|
| Short buffer write requests |—|30628|19428|-36.57%|

ON/XFS short remains-9.5668%; objective unmet. ON short samples
[145238.77,144058.30,144918.06], OFF[149052.76,141534.70,142046.51].
Actual short-phase calls1141–1144, subset hits1013–1019, prove the path ran.
Deferred counters include non-target data and metadata (including selected
metadata retained dirty), not a count of exclusively unrelated data bytes.
The request reduction is real; attributing the entire remaining deficit to
those requests is unsupported. Next diagnosis separates plan/data/stage/commit
time and actual background compaction work before another implementation change.

Selected NVMe restored to kernel nvme, no mount/holders/users, override(null),
node0 hugepages1024, immutable hashes and SMART unchanged, independent failed
dd-device soak preserved. Evidence: `build/test-results/fsync-scope-20261115/`.

### File-scope diagnostic decomposition

`fsync-scope-diag-20261115` passed exact-source timer-ON recovery75 commands,
then XFS profileOFF/ON3 each and NV scopeOFF/ON x profileOFF/ON3 each. All
payload/revision checks, fresh-process reopens, unchanged hashes and physical
cleanup passed. The common LevelDB Env wrapper records exact per-thread work
and samples read/append/flush timing1/64; filesystem timers split planning,
selected-data collection, staging and commit. Snapshots are outside timers and
can bracket concurrent background work. Different-thread inclusive times overlap;
committed-image read time is nested in planning, and engine stages nest in commit.

| Median3, profile OFF | XFS | Scope OFF | Scope ON |
|---|---:|---:|---:|
| Short batch100/100k ops/s |162791.07|142416.94|144673.25|
| Preceding single ops/s |32428.29|35233.45|37197.55|
| Single+short seconds |3.698370|3.540376|3.379562|

Scope ON/OFF short+1.5843%, ON/XFS-11.1295%; combined time-4.5423%.
This independent repetition preserves the earlier-9.5668% result rather than
replacing it with a selected favorable run. Scope remains experimental/defaultOFF.
ProfileON/OFF median short effects are XFS+0.156%, NV scopeOFF+1.228%, scopeON
+1.804%, within the material NV run variability; preceding single overhead is
about3-4%. Profiled throughput is diagnostic, not the adoption comparison.

In100k preceding single writes, scopeON profile medians are planning192.563ms
(committed reads38.138ms nested), selected-data750.616ms, staging21.048ms,
commit855.305ms. Engine log-write693.177ms is nested in the commit interval.
Background Flush's sampled inclusive estimate is1.390s and read0.700s, versus
XFS0.268/0.121s. Actual background work is335MiB read/384MiB append versus
XFS530/558MiB, so a faster foreground alone does not imply compaction caught up.
Short background work remains339/361MiB versus XFS257/308MiB. Its estimated
Flush/read intervals are192/118ms versus XFS146/59ms. Main short Sync is53ms
versus XFS89ms; the remaining short deficit is not simply a slower isolated
fsync call. Sampling and different actual work volumes limit direct attribution.

Next private candidate `fsync-overlap-20261115` combines the dependency-complete
file subset with the earlier bounded single cache executor during immutable
journal commit. Both settings stay defaultOFF. Its scope commit opens admission
only AFTER selected data is drained and private metadata is staged, closes and
waits BEFORE transaction restart/cache access. No journal image, barrier,
workload, compaction policy or disk layout changes. Three-file crash tests add
an unrelated pending allocation before the target fsync and a third-file append
during commit, then separately ACK the next file, requiring exact bitmap/free
counts after repeated original-image replay. Same-binary4arms independently
control executorOFF/ON and existing10/50us general-gate retry. Build passed;
combined real-primary recovery validation is running. No performance gain from
this candidate is claimed yet.

Diagnostic evidence archive SHA256
`0893cbaf9080d0a178fc2b88f690043e77b418131b185fef8f20202957c25cf0`;
local raw result and analysis: `build/test-results/fsync-scope-diag-20261115/`.

The combined candidate then passed102 exact-source recovery commands and65
full-regression/independent-checker outer commands, both with cleanup PASS.
All19 retained images passed independent replay and bitmap/reachability/free-
count checks. Three-file scenarios preserve A's acknowledged append while B's
pre-snapshot and C's concurrent allocations remain absent; a second C fsync
publishes exactly its2 pages after A's3 pages. Failed log publication restores
all original sizes/bytes and allocation counts. Original overlap close-waits,
truncate/unlink/fd-lifetime/AIO/stop exclusions, direct/indirect/three-page/
cold/short-write shapes, v2/v3 primary suites and buffered I/O fault regressions
passed. Sanitizer qualification remains as previously stated; the independent
server checker runs without unavailable GCC sanitizer runtime libraries.
Unprofiled factorial physical comparison has started; no adoption yet.
Archive SHA256ae6af38d6b71f70721373b9caaceb913fa3e1142ea46c9d39de3281378aa3f1e.
The candidate is implemented locally but defaultOFF and not adopted for the
headline comparison. No commit/push or unused-file cleanup.

### Scope plus immutable-commit overlap: first and reversed screens

Both tests reused one immutable binary, scope ON, profile OFF. General gate
retry10us plus overlap ON yielded165540.88ops/s versus fresh XFS162077.43
(+2.1369%, median3); scope-only144708.59, hence overlap gain14.3960%.
The50us overlap arm152413.14 remained5.9628% below XFS. This independently
controlled factorial result supersedes importing the old unscoped50us choice.

Reverse order (NV first/XFS last),5 full trials per arm, all payload/revision
oracles and reopens, source/binary hashes and cleanup PASS:

| Short batch100/100k | Scope only | Scope+overlap10us | XFS |
|---|---:|---:|---:|
| Median ops/s |145237.89|163722.52|161848.50|
| Mean ops/s |144373.344|161138.086|161832.810|

Candidate median beats XFS1.1579%, but mean trails0.4293%; raw candidate
samples[163722.52,156252.38,153620.80,165929.92,166164.81].
Do not claim robust completion from a favorable median. Combined preceding
single+short median3.144769s versus XFS3.696699s; short and combined metrics
are both retained. Wider same-binary adopted-baselineOFF versus candidateON
LevelDB/RocksDB short/long and fio functional regression is in progress.
No product overlap import/default change yet. Archive SHA256
997b2147689cf735c9bc1c954bfcecaf60453eae3c8a079f5884a56f6e70e131
(first), bf2042e7c0bf9c0756b6665f16dc7fa9ced8231cc19a55703870bd98f678b868
(reverse). Evidence under build/test-results/fsync-overlap-20261115/.

The wider regression's short LevelDB arm then completed with candidate samples
[158666.87,158193.15,166718.75] and fresh XFS
[161721.73,161624.36,161817.44]. Candidate median158666.87 is1.8890%
below XFS161721.73 (rounding provisional until complete report analysis).
Thus the earlier positive medians are not robust proof of completion. The full
LevelDB/RocksDB short/long and fio regression is still running; no candidate
is adopted. A separate defaultOFF data-completion overlap candidate is prepared
under build/test-results/fsync-data-overlap-20261115/: one bounded cache
executor only while scoped target-data completions are awaited, with all target
inode aliases excluded from append, and mandatory close/quiescence before
cache/job cleanup. Existing full collector/public ABI, snapshot and barriers
are retained. It waits for current measurements to finish before remote build,
dedicated alias/quiescence/ACK/failed-log recovery and independent checks.
Native non-SPDK archive build passed. No performance claim for the new path.

### First combined candidate: completed broad regression

All42 app trials (including smokes), complete revision/payload oracles and
fresh-process reopens PASS. Both settings passed32MiB CRC write/read and
all six fio functional profiles; source/binary hashes and cleanup PASS.

| App / batch operations | XFS short ops/s | Existing baseline | Scope+commit overlap | Candidate/baseline | Candidate/XFS |
|---|---:|---:|---:|---:|---:|
|level/100000|161721.73|140415.25|158666.87|+12.998%|-1.889%|
|level/1000000|124205.06|132931.34|133696.56|+0.576%|+7.642%|
|rocks/100000|187402.22|187711.17|184807.19|-1.547%|-1.385%|
|rocks/1000000|186621.34|180913.78|181099.95|+0.103%|-2.959%|

LevelDB preceding single improves12.2–12.4% versus existing baseline;
RocksDB preceding single regresses3.2–3.3%. Combined single+short time
changes-10.871%/-3.372% for LevelDB short/long, +2.965%/+0.848%
for RocksDB. These are product-relevant regressions, not just a positive
LevelDB headline. The first candidate remains unadopted. New data-wait
admission must be measured and wider regressions repeated if selected.
Archive SHA256317be19de817cf94bb5bbad5205e9436f0b7964fccbaefd5f285721ac3106608;
raw report and analysis under build/test-results/fsync-overlap-20261115/.

The next data-completion window candidate passed122 exact-source commands and
73 full-regression/independent-checker outer commands, both cleanup PASS.
All23 retained images passed independent replay, reachability/bitmap/free-count
checks. New tests exclude two aliases of the data-sync target while unrelated
cache append/read work progresses; owner close waits for a held client before
cache/job cleanup. ACK, next-file ACK and failed-log outcomes reopen repeatedly.
The first runner invocation stopped before tests because it retained the old
experiment directory; path correction and failed driver output are preserved,
with existing results untouched. Exact primary sources/binaries did not change.
Physical data-windowOFF/ON5 per arm and fresh XFS5 has started. Scope and
immutable-commit overlap stay ON in both NV arms, gate10us fixed. No adoption
or gain claimed yet. Source archive SHA256
b4736b2b7bb6167b7dcd9cb1057a4f0cf8b2e4f515e19a43c7542bd93ec7a6db.

### Data-completion overlap: first5-per-arm physical comparison

All payload/revision oracles, fresh-process reopens, full source/library/app
hashes and selected-device cleanup PASS. Scope and immutable-commit overlap
remain ON, general gate10us, only selected-data completion admission varies.
Same workload/WAL/sync=true, fixed preceding operations, profile OFF.

| Median5 | Fresh XFS | Data window OFF | Data window ON |
|---|---:|---:|---:|
| Short batch100/100k ops/s |162012.83|157211.34|177186.71|
| Preceding single ops/s |32325.60|39338.92|39447.12|
| Single+short seconds |3.712274|3.173884|3.099415|

Short ON/OFF+12.7061%, ON/XFS+9.3659%. Mean ON175097.944 versus
XFS161304.906 (+8.550% approximately); every ON sample exceeds every
XFS sample. ON[171671.80,169950.97,178132.98,178547.26,177186.71];
OFF[161230.78,157211.34,155503.47,164569.71,151411.71];
XFS[158669.21,161504.34,162095.65,162242.50,162012.83].
This first data-window screen is stronger than the commit-only positive
medians but does not yet qualify final default adoption or other-app regressions.
Unrelated cache-only operations demonstrably execute in the new window;
short-phase data-window reads2430–2709, writes2976–3351, bytes12399437–13961155
across the5 trials. No target alias append is admitted while selected data is
FLUSHING; existing immutable-commit admission remains separately bounded.

Next isolated runtime-only comparison checks an avoidable duplicated gate
retry before adopting the combined path. The previous code spins its explicit
retry budget and then calls lock_fs_gate, which can spin that budget again;
even writes larger than the8KiB admission limit pay the first loop. Compare
existing baseline, selected data+commit overlap with original retry, and the
same overlap with one retry budget. All core/journal/scope source hashes remain
identical to the122+73-command candidate. Retest changed-runtime data/commit
admission, lifecycle/quiescence/fault and independent retained-image replay;
then compare LevelDB and RocksDB short under identical settings. No write-limit,
workload, compaction policy or durability change. This gate hypothesis has
not yet been measured; no gain is attributed to it.

Data-window evidence archive SHA256
73df2b9fbff6d7483747c93199b04177256116fa599a2e5356729142e3f71c32;
raw result and analysis under build/test-results/fsync-data-overlap-20261115/.

### Gate retry policy comparison and final integration candidate

The isolated single-retry gate change passed42 runtime/admission/fault/checker
commands, but its matched three-per-arm physical comparison does not justify
adoption. All32 app trials including smokes, full revision/payload checks,
fresh-process reopens, source/binary hashes and device cleanup PASS.

| Median3 short ops/s | XFS | Previous optimization baseline | Scope+commit+data, original retry | Same path, single retry |
|---|---:|---:|---:|---:|
| LevelDB100k |161685.12|143830.95|177203.84|171178.17|
| RocksDB100k |188762.98|186627.24|184336.30|185720.37|

Single retry changes LevelDB short by-3.4004% and RocksDB short by+0.7508%
relative to original retry. Preceding single changes-0.9749%/+0.1252%.
The duplicated spin hypothesis did not explain the remaining RocksDB regression;
it is not retained in product code. Original retry wins the focused LevelDB
goal by+9.5981% versus fresh XFS, while RocksDB short remains-2.3451% versus
XFS and-1.2276% versus the previous baseline. Do not generalize the LevelDB
win to RocksDB or all applications.

Evidence: build/test-results/fsync-data-gate-20261115/{result,analysis}.json
and evidence.tar.gz, archive SHA256
`d11be200f39aaa2f0167312d67baa2816f5b0ab417ebc5d8d7a15d023638a5b3`.

Sixteen reviewed source/test files have now been integrated into the current
checkout without changing prior optimizations. Scoped fsync and bounded
immutable-commit/data-wait cache admission default ON only under their existing
standalone/v3/ownership eligibility checks; unsupported paths still fall back.
The rejected retry switch was removed. Default file format remains unchanged;
existing v2 volumes are not upgraded. `NVFUSE_KEEP_DIRTY_BH_IN_ICTX` disables
the unsupported subset/admission combination. Read-only statistics snapshots
replace private library print helpers; workload source remains byte-identical.

Exact final source archive SHA256
`094a88d4fadfc06539275bb07f5b1468c433ea62d856ac2d87ca4a368fb071be`.
Native journal-v3, journal/transaction and journal-boundary suites PASS.
Real-SPDK rebuild, exact/default regression, broad same-binary comparison and
reversed target reproduction are pending. No final completion claim yet.

Final source SPDK build PASS (18 build commands). Exact qualification passed
110 scope/combined/data-window commands, then the runner appended DATA_CACHE=0
after taskset, so lifecycle `cc-format` did not execute (exit127). The22
successfully formatted images, the unformatted failed-launch image, original
failed report and logs remain intact; hugepages restored. Corrected resume
passed the remaining12 lifecycle/shapes/ACK/fault commands on the same binaries.
A second full-runner attempt stopped in preflight because it still required
the deliberately preserved failed report to say PASS; no full-regression command
ran in that attempt. Corrected full-runner preflight uses the12-command resume
plus the original110 successful command records. This is122 successful exact
commands across the two actual runs, not a claim that the first report passed.
Full/default v2/v3/cache/AIO/fault tests and independent checks of the22 original
successful images plus the new lifecycle image are now running in
`validation-full-v3`. No product source/binary changes were needed for the
runner corrections; all failed attempts and unused runner bundles are retained.

Final `validation-full-v3` PASS:73 outer commands, including independent replay/checker of23 retained images and default-settings v2/v3 primary/cache/AIO/fault suites. Cleanup PASS. Final actual-default broad application/fio comparison now starts on the approved4d device; source/binaries remain unchanged.

### Final integrated defaults: completed broad comparison

All42 application trials including smokes, full payload/revision checks and fresh-process reopens PASS. OFF and actual-default settings each passed32MiB CRC write/read plus all six fio functional profiles. Source/library/app hashes, selected-device cleanup and retained unrelated failed-soak state PASS. ON measurements explicitly unset all three new settings and the general-gate override; this exercises startup defaults, not only explicit opt-in. Workload source and compaction/WAL/sync policy are unchanged.

| Median3 batch100 operations/s | XFS | Previous optimized baseline OFF | Integrated defaults | Default/baseline | Default/XFS |
|---|---:|---:|---:|---:|---:|
|level/100000|159235.70|143719.84|171847.04|+19.5709%|+7.9199%|
|level/1000000|123979.49|132163.50|138496.13|+4.7915%|+11.7089%|
|rocks/100000|188447.37|186730.29|186115.46|-0.3293%|-1.2374%|
|rocks/1000000|186718.02|179477.51|184484.49|+2.7898%|-1.1962%|

For focused LevelDB100k, all three default samples exceed all three fresh XFS
samples; mean advantage+9.0474%. Default raw[171074.38,171847.04,178561.65],
XFS[158039.79,159235.70,160941.30], baseline[144146.34,140600.24,143719.84].
The preceding single phase improves11.46%/11.99% versus baseline for LevelDB
short/long. Combined single+batch times improve11.35%/6.35%.

RocksDB single-update throughput regresses2.83%/3.16% versus baseline, while
remaining24–25% above XFS. Its short batch regresses0.3293% versus baseline;
long batch improves2.7898%. Combined time changes+2.4867%/-0.8480%.
These remaining regressions are retained; the LevelDB target does not imply
all applications or all fio profiles exceed XFS. No new headline fio throughput
comparison was run in this fsync-specific final stage; its fio tests are
functional regression only. The earlier equal-volume fio criterion remains.

Broad evidence archive SHA256
`5b94f67650814722fda0671ed1428bfa12368321f33519fc8a12c41e0b03ad7a`;
raw result SHA256
`00d3c4cbf0395b0d8fa9a8de99f80b3152ff9aba289cd4f4cbbfe32b33b16429`.
Local raw, analysis, source/runner archives and all logs are under
`build/test-results/fsync-final-20261115/`. The exact final binary now undergoes
reversed-order LevelDB short5-per-arm versus fresh XFS5. Final target acceptance
is pending that confirmation and a final live audit.

### Final same-binary reverse confirmation and delivery

NVFUSE-first, then fresh XFS,5 full trials per arm; all18 trials including smokes and every reopen PASS. Median short 177395.23 versus XFS160136.13 (+10.7778%); same-binary prior baseline144040.15, default/baseline+23.1568%. Mean advantage+9.3834%. Combined preceding-single+short time3.110058s versus baseline3.537738s and XFS3.709843s.

level/100000/nvfuse/1: [174502.34, 167390.79, 177395.23, 177552.56, 178768.01]

level/100000/nvfuse/0: [147954.6, 143044.67, 144040.15, 144224.0, 143501.66]

level/100000/xfs/0: [160136.13, 158716.32, 159638.97, 160291.61, 161712.55]

All5 new-default samples exceed all5 fresh XFS samples. Source/binary hashes,
selected-device cleanup and the final independent live audit PASS. The raw
reverse report inherited the broad runner's descriptive application metadata;
its actual benchmark rows contain only LevelDB short. A separate
`reverse-evidence/measured-scope-note.json` records that correction and the
unmodified raw report hash; no RocksDB/long/fio reverse measurement is claimed.

Reverse evidence SHA256
`fa137b15e3867096ce3d097d466d88a4cf7140d4e34f9a7e5401f1ee0a410775`;
raw SHA256
`62b483ac57ebaab325616085073a41ea9e69d11ee8d7a8d410623caf87b3ff0e`;
final live audit SHA256
`47d3ef7cbd455ec86a348d92849c8c7344e76cdd3ad471b115c68fe101fe0b7d`.

The focused fsync/LevelDB short target is complete. Wider all-profile XFS parity
and the recorded RocksDB/single-update and baseline truncate-gap issues remain
open. The original layout and prior optimizations remain intact; rejected gate
retry policy is absent. No commit/push/release or unused-file cleanup occurred.
