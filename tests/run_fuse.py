#!/usr/bin/env python3
"""Mounted Linux POSIX smoke tests and completed-operation crash recovery."""
import argparse
import datetime
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PAYLOAD = bytes(range(256)) * 8193 + b"unaligned-tail"


def expect(code, operation):
    try:
        operation()
    except OSError as error:
        assert error.errno == code, (error, code)
    else:
        raise AssertionError(f"expected errno {code}")


def operations(root, stage):
    folder = root / "POSIX tests"
    file = folder / "binary.dat"
    if stage == "write":
        folder.mkdir()
        with file.open("wb") as stream:
            stream.write(PAYLOAD)
            stream.flush()
            os.fsync(stream.fileno())
        assert file.read_bytes() == PAYLOAD
        os.chmod(file, 0o640)
        os.utime(file, (123456, 234567))
        st = file.stat()
        assert st.st_uid == os.getuid() and st.st_gid == os.getgid()
        assert st.st_mode & 0o777 == 0o640
        assert int(st.st_atime) == 123456 and int(st.st_mtime) == 234567
        os.setxattr(file, "user.binary", bytes(range(32)))
        assert os.getxattr(file, "user.binary") == bytes(range(32))
        assert "user.binary" in os.listxattr(file)
        os.setxattr(file, "user.empty", b"")
        assert os.getxattr(file, "user.empty") == b""
        os.removexattr(file, "user.empty")
        expect(errno.ENODATA, lambda: os.getxattr(file, "user.empty"))
        (folder / "symlink").symlink_to("binary.dat")
        assert os.readlink(folder / "symlink") == "binary.dat"
        assert (folder / "symlink").read_bytes() == PAYLOAD
        os.link(file, folder / "hardlink")
        assert (folder / "hardlink").stat().st_ino == st.st_ino
        with (folder / "append").open("wb") as stream:
            stream.write(b"A")
        with (folder / "append").open("ab") as stream:
            stream.write(b"B")
        assert (folder / "append").read_bytes() == b"AB"
        with (folder / "append").open("wb") as stream:
            stream.write(b"C")
        assert (folder / "append").read_bytes() == b"C"
        with (folder / "sparse").open("w+b") as stream:
            stream.seek(100003)
            stream.write(b"Z")
            stream.truncate(200007)
            stream.flush()
            stream.seek(99999)
            assert stream.read(5) == b"\0\0\0\0Z"
            stream.seek(200000)
            assert stream.read() == b"\0" * 7
        gone = folder / "unlinked"
        with gone.open("w+b") as stream:
            stream.write(b"still open")
            stream.flush()
            gone.unlink()
            stream.seek(0)
            assert stream.read() == b"still open"
            assert os.fstat(stream.fileno()).st_size == 10
            os.ftruncate(stream.fileno(), 4)
            assert os.fstat(stream.fileno()).st_size == 4
        old = folder / "replace"
        old.write_bytes(b"old inode")
        with old.open("rb") as stream:
            replacement = folder / "replacement"
            replacement.write_bytes(b"new inode")
            os.replace(replacement, old)
            assert stream.read() == b"old inode"
            assert old.read_bytes() == b"new inode"
        many = folder / "many"
        many.mkdir()
        for i in range(150):
            (many / f"entry-{i}").touch()
        for i in range(0, 150, 3):
            (many / f"entry-{i}").unlink()
        assert set(os.listdir(many)) == {f"entry-{i}" for i in range(150) if i % 3}
        expect(errno.EEXIST, lambda: folder.mkdir())
        expect(errno.ENOTEMPTY, lambda: folder.rmdir())
        assert os.statvfs(root).f_bsize > 0
        subprocess.run(["cp", "--preserve=mode,timestamps,xattr", str(file), str(folder / "copy")],
                       check=True, timeout=20)
        assert (folder / "copy").read_bytes() == PAYLOAD
        assert os.getxattr(folder / "copy", "user.binary") == bytes(range(32))
        subprocess.run(["mv", str(folder / "copy"), str(folder / "moved")], check=True, timeout=20)
        assert (folder / "moved").read_bytes() == PAYLOAD
    else:
        assert hashlib.sha256(file.read_bytes()).digest() == hashlib.sha256(PAYLOAD).digest()
        assert os.getxattr(file, "user.binary") == bytes(range(32))
        assert os.readlink(folder / "symlink") == "binary.dat"
        assert (folder / "replace").read_bytes() == b"new inode"
        assert len(os.listdir(folder / "many")) == 100
        assert (folder / "moved").read_bytes() == PAYLOAD
    print(f"PASS mounted {stage}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/fuse/nvfuse-fuse")
    parser.add_argument("--worker", choices=("write", "verify"))
    parser.add_argument("--mountpoint", type=Path)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    if args.worker:
        operations(args.mountpoint, args.worker)
        return
    if sys.platform != "linux" or not Path("/dev/fuse").exists() or not shutil.which("fusermount3"):
        parser.error("Linux, /dev/fuse and fusermount3 are required")
    root = (args.report_dir or Path("build/test-results") /
            datetime.datetime.now().strftime("fuse-%Y%m%d-%H%M%S-%f")).resolve()
    root.mkdir(parents=True, exist_ok=False)
    image, mountpoint = root / "disk.img", root / "mount"
    mountpoint.mkdir()
    binary = str(Path(args.binary).resolve())
    process, log = None, None
    report = {"status": "running", "stages": [], "platform": os.uname()._asdict() if hasattr(os.uname(), "_asdict") else list(os.uname())}
    try:
        with (root / "mkfs.log").open("wb") as output:
            subprocess.run([binary,"--mkfs",str(image),"--size-mib","256"],
                           stdout=output,stderr=subprocess.STDOUT,check=True,timeout=60)
        digest = hashlib.sha256(image.read_bytes()).digest()
        again = subprocess.run([binary,"--mkfs",str(image),"--size-mib","256"], capture_output=True, timeout=30)
        assert again.returncode and hashlib.sha256(image.read_bytes()).digest() == digest
        for i, stage in enumerate(("write", "verify", "verify")):
            log = (root / f"mount-{i}.log").open("wb")
            process = subprocess.Popen([binary,"--image",str(image),str(mountpoint),"--debug"],
                                       stdout=log,stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while not os.path.ismount(mountpoint):
                if process.poll() is not None:
                    raise RuntimeError(f"mount exited {process.returncode}; see mount-{i}.log")
                if time.monotonic() > deadline:
                    raise TimeoutError("mount did not become ready")
                time.sleep(0.1)
            result = subprocess.run([sys.executable,__file__,"--worker",stage,"--mountpoint",str(mountpoint)],
                                    capture_output=True,timeout=90)
            (root / f"operations-{i}.log").write_bytes(result.stdout + result.stderr)
            if result.returncode:
                sys.stderr.buffer.write(result.stdout + result.stderr)
                raise RuntimeError(f"mounted {stage} exited {result.returncode}")
            report["stages"].append(stage)
            print(result.stdout.decode().strip(),flush=True)
            if i == 1:
                process.kill()
                process.wait(timeout=10)
                subprocess.run(["fusermount3","-uz",str(mountpoint)],check=True,capture_output=True,timeout=10)
            else:
                subprocess.run(["fusermount3","-u",str(mountpoint)],check=True,capture_output=True,timeout=10)
                assert process.wait(timeout=10) == 0
            process = None
            log.close()
            log = None
        report["status"] = "pass"
        image.unlink()
    except BaseException as error:
        report.update(status="fail",error=str(error))
        print(f"Failure artifacts preserved: {root}",file=sys.stderr)
        raise
    finally:
        if process is not None:
            subprocess.run(["fusermount3","-uz",str(mountpoint)],capture_output=True,timeout=10)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        if log is not None:
            log.close()
        (root / "summary.json").write_text(json.dumps(report,indent=2))
        print(f"Results: {root}",flush=True)


if __name__ == "__main__":
    main()
