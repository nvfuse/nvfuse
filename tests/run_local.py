#!/usr/bin/env python3
"""Run each persistence stage in a fresh process using disposable file images."""
import fcntl
import hashlib
import pathlib
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()

def run(mode, image, success=True):
    try:
        result = subprocess.run([str(binary), mode, str(image)], capture_output=True, timeout=45)
    except subprocess.TimeoutExpired as error:
        sys.stderr.buffer.write((error.stdout or b"") + (error.stderr or b""))
        raise
    if (success and (result.returncode or f"PASS {mode}".encode() not in result.stdout)
            or not success and result.returncode != 1):
        sys.stderr.buffer.write(result.stdout + result.stderr)
        raise RuntimeError(f"{mode} {image.name}: unexpected exit {result.returncode}")

def digest(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        return digest.hexdigest()

with tempfile.TemporaryDirectory(prefix="nvfuse-local-") as directory:
    root = pathlib.Path(directory)
    image = root / "disk.img"
    for mode in ("create", "verify", "reopen"):
        run(mode, image)
        print(f"PASS {mode}", flush=True)

    before = digest(image)
    run("create", image, success=False)
    assert digest(image) == before, "create overwrote an existing image"
    with image.open("r+b") as locked:
        fcntl.flock(locked, fcntl.LOCK_EX | fcntl.LOCK_NB)
        run("verify", image, success=False)
    assert digest(image) == before, "locked image was modified"
    link = root / "link.img"
    link.symlink_to(image)
    run("verify", link, success=False)
    assert digest(image) == before, "symlink target was modified"
    print("PASS existing-image, lock and symlink protection", flush=True)

    invalid = root / "invalid.img"
    for size in (4096, 128 * 1024 * 1024 + 1, 128 * 1024 * 1024):
        with invalid.open("wb") as stream:
            stream.truncate(size)
        before = digest(invalid)
        run("verify", invalid, success=False)
        assert digest(invalid) == before, "invalid image was modified"
    run("verify", root, success=False)
    run("verify", root / "missing.img", success=False)
    print("PASS invalid image rejection", flush=True)

    for mode in ("mutate", "final", "final"):
        run(mode, image)
        print(f"PASS {mode}", flush=True)
print("All local file-backend tests passed.")
