#!/usr/bin/env python3
"""Exercise actual Linux FUSE callbacks and reopen images in fresh processes."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
root = Path(tempfile.mkdtemp(prefix="nvfuse-fuse-adapter-"))
try:
    for mode in ("create", "verify"):
        result = subprocess.run([binary, mode, str(root / "disk.img")],
                                capture_output=True, timeout=120)
        (root / f"{mode}.log").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            sys.stderr.buffer.write(result.stderr[-4000:])
            raise RuntimeError(f"{mode}: exit {result.returncode}")
        print(f"PASS FUSE adapter {mode}", flush=True)
except BaseException:
    print(f"Failure artifacts preserved: {root}", file=sys.stderr)
    raise
else:
    shutil.rmtree(root)
