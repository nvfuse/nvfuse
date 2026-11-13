#!/usr/bin/env python3
"""Guest-side real API/FUSE interoperability tests on disposable storage."""
import argparse
import datetime
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
sys.dont_write_bytecode = True
from run_fuse import operations


def mounted_worker(root, stage):
    data = (root / "api-data").read_bytes()
    assert len(data) == 2 * 1024 * 1024 + 137
    assert data == bytes((i * 37 + i // 4096) & 255 for i in range(len(data)))
    assert os.getxattr(root / "api-data", "user.binary") == b"\0\xff\1\x80"
    assert not (root / "aborted").exists()
    operations(root, stage)


def main():
    def stop(signum, frame):
        raise InterruptedError(f"received signal {signum}")

    signal.signal(signal.SIGTERM, stop)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("imagefile", "kernel", "spdk"), default="imagefile")
    parser.add_argument("--target", help="SPDK BDF, guarded by the QEMU guest wrapper; kernel tests allocate their own loop device")
    parser.add_argument("--api-binary", default="build/fuse/backend_test")
    parser.add_argument("--fuse-binary", default="build/fuse/nvfuse-fuse")
    parser.add_argument("--report-dir", type=Path)
    parser.add_argument("--worker", choices=("write", "verify"))
    parser.add_argument("--mountpoint", type=Path)
    args = parser.parse_args()
    if args.worker:
        mounted_worker(args.mountpoint, args.worker)
        return
    if sys.platform != "linux":
        parser.error("run inside Linux")
    if args.target and args.backend != "spdk":
        parser.error("only SPDK accepts --target; kernel/image tests create disposable storage")
    if args.backend == "spdk":
        if not args.target or os.environ.get("NVFUSE_TEST_BDF") != args.target:
            parser.error("SPDK tests require the dedicated-device guest wrapper")
        pci = Path("/sys/bus/pci/devices") / args.target
        if (pci / "vendor").read_text().strip() != "0x1b36" or (pci / "device").read_text().strip() != "0x0010":
            parser.error("test target is not the QEMU NVMe fixture")
    root = (args.report_dir or Path("build/test-results") /
            datetime.datetime.now().strftime(f"backend-{args.backend}-%Y%m%d-%H%M%S-%f")).resolve()
    root.mkdir(parents=True, exist_ok=False)
    mountpoint, image = root / "mount", root / "disk.img"
    mountpoint.mkdir()
    api, fuse = str(Path(args.api_binary).resolve()), str(Path(args.fuse_binary).resolve())
    report = {"status": "running", "backend": args.backend, "uname": list(os.uname()), "stages": []}
    process, mount_log, loop = None, None, None

    def run(name, command, timeout=90, expected=0):
        with (root / (name + ".log")).open("wb") as log:
            result = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        report["stages"].append({"name": name, "command": [str(x) for x in command], "returncode": result.returncode})
        if result.returncode != expected:
            raise RuntimeError(f"{name} failed ({result.returncode}); see log")
        print(f"PASS {args.backend} {name}", flush=True)

    def unmount(crash=False):
        nonlocal process, mount_log
        if crash:
            process.kill()
            process.wait(timeout=10)
        subprocess.run(["fusermount3", "-uz" if crash else "-u", str(mountpoint)], capture_output=True, check=True, timeout=10)
        if not crash and process.wait(timeout=15) != 0:
            raise RuntimeError("mount process failed on close")
        process = None
        mount_log.close()
        mount_log = None

    try:
        target = str(image)
        if args.backend == "kernel":
            with image.open("xb") as stream:
                stream.truncate(256 * 1024 * 1024)
            loop = subprocess.check_output(["losetup", "--find", "--show", "--nooverlap", str(image)], timeout=10).decode().strip()
            backing = Path("/sys/class/block") / Path(loop).name / "loop/backing_file"
            assert Path(backing.read_text().strip()).resolve() == image
            target = loop
        elif args.backend == "spdk":
            target = args.target
        report["target"] = target
        # CLI must reject a device format without --force, before opening I/O.
        if args.backend != "imagefile":
            run("reject-format-without-force", [fuse, "--backend", args.backend, "--target", target,
                                                 "--mkfs", "--size-mib", "256"], expected=2)
        if args.backend == "kernel":
            run("reject-unformatted-reopen", [api, "verify", "kernel", target], expected=1)
            # A reopen of blank storage must not silently turn into a format.
            with image.open("rb") as stream:
                assert all(chunk == bytes(len(chunk)) for chunk in iter(lambda: stream.read(1024 * 1024), b""))
        run("api-create", [api, "whole-create" if args.backend == "kernel" else "create", args.backend, target])
        run("api-reopen", [api, "verify", args.backend, target])
        for index, stage in enumerate(("write", "verify", "verify")):
            mount_log = (root / f"mount-{index}.log").open("wb")
            process = subprocess.Popen([fuse, "--backend", args.backend, "--target", target, str(mountpoint)],
                                       stdout=mount_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 30
            while not os.path.ismount(mountpoint):
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("mount failed; see mount log")
                time.sleep(.05)
            if index == 0:
                run("exclusive-open", [api, "verify", args.backend, target], expected=1)
            run(f"mounted-{stage}-{index}", [sys.executable, __file__, "--worker", stage,
                                               "--mountpoint", mountpoint])
            unmount(crash=index == 1)
        run("api-read-fuse-data", [api, "after-fuse", args.backend, target])
        if loop:
            subprocess.run(["losetup", "-d", loop], check=True, timeout=10)
            loop = None
            run("kernel-to-image-reopen", [api, "after-fuse", "imagefile", image])
        report["status"] = "pass"
    except BaseException as error:
        report.update(status="fail", error=repr(error))
        raise
    finally:
        try:
            if process is not None:
                subprocess.run(["fusermount3", "-uz", str(mountpoint)], capture_output=True, timeout=10)
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=15)
            if mount_log:
                mount_log.close()
            if loop:
                subprocess.run(["losetup", "-d", loop], check=True, timeout=10)
            if report["status"] == "pass" and image.exists():
                image.unlink()
        finally:
            (root / "summary.json").write_text(json.dumps(report, indent=2))
            print(f"Results: {root}", flush=True)


if __name__ == "__main__":
    main()
