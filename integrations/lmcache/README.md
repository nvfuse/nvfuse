# LMCache integration

Pinned LMCache is `0.5.0.dev20261114` at the source revision in
[versions.json](../versions.json). The reference uses its native-plugin factory
and `NativeConnectorL2Adapter`. The exercised Python environment used Python
3.11 and torch 2.6.0+cpu. This is a CPU storage adapter; GPU serving is a separate
integration and performance exercise.

## Build and configure

Build the core archive first, then from the repository root:

```sh
make -C integrations lmcache SPDK_ROOT_DIR="$SPDK_SOURCE"
export PYTHONPATH="$PWD/integrations/lmcache${PYTHONPATH:+:$PYTHONPATH}"
```

Install the pinned LMCache source and its own dependencies in a separate Python
environment. [config.example.json](config.example.json) supplies the connector
fields; replace its library path with the absolute
`integrations/common/libnvfuse_ai.so` path and incorporate it into the pinned
LMCache configuration. The example uses FIFO to keep scheduling explicit.
Set the NVFUSE bdev/runtime environment from the core user guide. The connector
reopens the volume and never formats it; its arena file is created/expanded and
written for cache objects, so choose a private path with sufficient space.

`lmcache_nvfuse.py` is the zlib CRC connector. `lmcache_nvfuse_accel.py` and
`crc_isal.py` provide an optional ISA-L frontend CRC path selected by the module
name and `NVAI_CRC_LIBRARY`. The library must export `crc32_gzip_refl`. Keep the
checksum implementation fixed across core comparisons; CRC acceleration is not
a filesystem-core change. Use the default zlib path if that symbol is absent.

## Focused verification

After deliberate volume preparation, run from the repository root with the
runtime environment set. Output paths belong under ignored `build/`:

```sh
mkdir -p build/integration-checks
PYTHONPATH=integrations/lmcache python3 integrations/lmcache/connector_checks.py \
  --backend nvfuse --arena /lmcache-check.arena \
  --library "$PWD/integrations/common/libnvfuse_ai.so" \
  --output build/integration-checks/lmcache-connector.json
PYTHONPATH=integrations/lmcache python3 integrations/lmcache/batch_checks.py \
  --backend nvfuse --arena /lmcache-batch.arena \
  --library "$PWD/integrations/common/libnvfuse_ai.so" \
  --output build/integration-checks/lmcache-batch.json
PYTHONPATH=integrations/lmcache python3 integrations/lmcache/lmcache_bridge_check.py \
  --backend nvfuse --arena /lmcache-bridge.arena \
  --library "$PWD/integrations/common/libnvfuse_ai.so" \
  --output build/integration-checks/lmcache-bridge.json
```

The first checks capacity/reclamation and close/drain; the second checks dependent
FIFO overwrites and full payloads. The third requires the actual pinned LMCache
and torch and checks its factory/ObjectKey/tensor bridge. A mock or connector-only
pass does not establish LMCache end-to-end support.

The key index is process-local. Restarting a connector gives cache misses for
old slots; this is a disposable cache, not a persistent key/value database.
Close the connector to drain accepted work before stopping the process. Linux
`eventfd` and the pinned native-plugin API are required. The focused release checks exercise the pinned CPU bridge; they do not qualify
a GPU inference server.
