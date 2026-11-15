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
python3 tools/check_reader_symbols.py "$DISKANN_EXTENSION"
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
python3 tools/shutdown_reader.py "$SHORT_PRIVATE_SOCKET_PATH"
```

Require success before stopping or rebinding the device. Restart the process
for another reader lifecycle. Use the actual application search/recall check;
there is no invented generic DiskANN server command in this package.

Run tool commands from `integrations/` (set `ADAPTER_DIR` to that directory).
Build from the repository root with `make -C integrations ann SPDK_ROOT_DIR="$SPDK_SOURCE"`
after building the core. The reader source is shared in `common/`; its new
layout still requires Linux build/load and application qualification.
