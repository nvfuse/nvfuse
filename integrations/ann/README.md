# DiskANN graph reader

Pinned consumer: diskannpy 0.7.0, DiskANN source
`df225d32da81db87596a3ce7c040ad0f78709a7c`. Use the C++11 libstdc++ string ABI and
exported `LinuxAlignedFileReader` open/close/blocking-read entry points recorded
in `../versions.json`. Rust DiskANN and async-only/AiSAQ methods are outside
this integration.

Build using the integration index and follow the shared
[graph import procedure](../common/README.md#import-an-immutable-graph).
Check the actual diskannpy extension with:

```sh
python3 integrations/tools/check_reader_symbols.py "$DISKANN_EXTENSION"
```

Import the padded graph and review a route such as the parent directory's
`ann/routes.example.tsv`; choose a unique suffix matching the actual index
file. Keep auxiliary files and the original graph header readable through POSIX.

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

Start the pinned application with these variables. After queries finish, close
all DiskANN readers and then run:

```sh
python3 integrations/tools/shutdown_reader.py "$SHORT_PRIVATE_SOCKET_PATH"
```

Require success before stopping or rebinding the device. Restart the process
for another reader lifecycle. Use the actual application search/recall check;
there is no invented generic DiskANN server command in this package.

Run the commands on this page from the repository root.
Build from the repository root with `make -C integrations ann SPDK_ROOT_DIR="$SPDK_SOURCE"`
after building the core. The reader source is shared in `common/`; qualify the
resulting library against the pinned application before use.

## Small correctness example

Use Python 3.11 with the pinned wheel and NumPy ABI, in a separate environment:

```sh
python3.11 -m venv build/ann-venv
build/ann-venv/bin/python -m pip install numpy==1.25.0 diskannpy==0.7.0
ANN_PY="$PWD/build/ann-venv/bin/python"
"$ANN_PY" integrations/ann/smoke.py prepare \
  --directory build/ann-demo --output build/integration-checks/ann-prepare.json
"$ANN_PY" integrations/ann/smoke.py search \
  --directory build/ann-demo --output build/integration-checks/ann-posix.json
```

This generates 10,000 64-dimensional vectors, a real immutable disk index and
exact top-10 ground truth for 16 queries. Cache mechanism 0 and zero cached graph
nodes keep graph reads visible. The required recall@10 is 0.95; this small data
set is a correctness fixture, not a performance or SSD saturation benchmark.
Preparation refuses an existing fixture directory.

Build the native reader and prepare an exclusively owned NVFUSE volume using
the core guide. Keep `NVFUSE_SPDK_JSON`, `NVFUSE_SPDK_BDF`, `NVFUSE_SPDK_BDEV`,
core mask and required device/hugepage permissions set for the following commands.
Normal import and search reopen the volume without formatting:

```sh
make -C integrations ann SPDK_ROOT_DIR="$SPDK_SOURCE"
python3 integrations/tools/import_graph.py \
  --library "$PWD/integrations/common/libnvfuse_ai.so" \
  --file "$PWD/build/ann-demo/release_disk.index=/ann-release.graph" \
  --output build/integration-checks/ann-import.json
printf 'release_disk.index\t/ann-release.graph\n' > build/ann-demo/routes.tsv
NVAI_READER_MAP="$PWD/build/ann-demo/routes.tsv" \
NVAI_READER_EVIDENCE="$PWD/build/integration-checks/ann-native-reader.jsonl" \
LD_PRELOAD="$PWD/integrations/common/libnvfuse_diskann.so" \
  "$ANN_PY" integrations/ann/smoke.py search \
  --directory build/ann-demo --output build/integration-checks/ann-native.json
```

Require full-byte import/readback, recall success, positive native reader bytes
and reads, and a clean shutdown record in the JSON report. The smoke program
closes its actual DiskANN reader and explicitly shuts down the native runtime.
Repeat the final command in a new process with new evidence/output filenames
for a persisted-data reopen check. Keep auxiliary/PQ files and the original
POSIX graph header in place. Check the extension's symbols as described above.
