# Release notes

## Unreleased — modern API/SPDK prerelease

This source release is intended for development and evaluation. A release tag
has not been assigned. Production stability and physical power-loss durability
are not qualified.

### Interfaces and storage

- Direct C API + SPDK is the primary performance path, with a shared core
  library, a serialized filesystem executor and asynchronous NVMe I/O reactor.
- Linux FUSE 3 provides compatibility access for ordinary applications. It uses
  the standalone runtime and synchronous transport, including with SPDK.
- Standalone APIs support image files on macOS/Linux and Linux kernel block
  devices or SPDK namespaces. These are distinct from the primary reactor.
- Device formatting uses the selected device's capacity rounded down to
  complete 128 MiB block groups. All interfaces require exclusive ownership;
  API and FUSE may alternate after clean close/unmount.

### Journaling and compatibility

- Primary metadata journaling defaults ON. New journal formats default to v2.
  Compact append-log v3 is experimental and requires explicit format selection.
- Reopen uses the stored journal version. There is no automatic upgrade or
  in-place conversion between legacy, v1, v2 and v3 volumes. Older binaries
  can reject newer formats. Formatting is destructive, not a recovery method.
- B+tree directory indexes, the inode/indirect-block structure, block groups,
  caches and control-plane design remain the foundation. Journaling reserves
  disk space and adds versioned discovery/recovery structures. Existing native
  home structures do not establish cross-platform image interchange.
- Standalone application metadata/DATA transactions have their own contract;
  primary metadata journaling does not imply that API is available there.

### Integration and documentation

- Modern fio integration, live `nvfuse-stat` monitoring and buffered API/SPDK
  I/O share the core runtime. Database performance measurements use prototype
  adapters; a production database integration is not included.
- Implementations now live in `src/`, with private helpers in
  `src/spdk_runtime/`. Headers remain in `include/`; documented build commands
  and modern archive paths remain unchanged.
- User/validation guides, an image API example, GPL license texts and expanded
  third-party notices accompany the source. Local research and run artifacts
  are excluded. See [release preparation](docs/releasing.md).

### Known limits

- Primary API/SPDK targets Linux x86_64. Its current x86 CRC helpers and build
  settings prevent an unmodified primary build in the ARM64 development VM.
  Native image support on macOS does not imply an ARM64 primary SPDK port.
- FUSE offers documented basic POSIX compatibility, not full conformance;
  macFUSE/Finder mounting and Windows are outside this release's support scope.
- Some allocation/cache/error paths still abort, including primary full-ENOSPC
  cases. Near-full stress is not complete-exhaustion qualification.
- The current mapper limits device formats to approximately 8 TiB. Primary
  fsync/fdatasync commits filesystem-wide; ordinary completion is not a sync.
- Physical power-cut testing, complete device-loss/error handling,
  same-directory contention and long-duration qualification remain open.
- README performance numbers are historical v3-ON development measurements,
  not default-v2, equal-CPU/cache, FUSE or universal XFS-parity claims.

See [TODO](TODO) and the [validation guide](docs/testing-guide.md) for remaining
qualification work. GPL v2 remains the project license; see [NOTICE](NOTICE)
for file-specific provenance and attribution.
