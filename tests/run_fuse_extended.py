#!/usr/bin/env python3
"""Reference-based mounted compatibility, stress, crash and fio measurements.

Run in Linux. All workloads use newly created directories and an image file;
no block device, existing mount, or host cache setting is modified.
"""
import argparse
import datetime
import errno
import hashlib
import json
import mmap
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time


def payload(index):
    return hashlib.sha256(str(index).encode()).digest() * 2048


def compatibility(root):
    results = {}

    def probe(name, fn):
        try:
            results[name] = {"value": fn()}
        except OSError as error:
            results[name] = {"errno": error.errno}
        except Exception as error:
            results[name] = {"error": repr(error)}

    def basic():
        p = root / "basic"
        p.write_bytes(b"hello")
        os.link(p, root / "hard")
        (root / "sym").symlink_to("basic")
        result = [(root / "sym").read_bytes().decode(), p.stat().st_nlink,
                  (root / "hard").stat().st_ino == p.stat().st_ino]
        return result

    def xattr():
        p = root / "basic"
        os.setxattr(p, "user.test", b"\0\xff", flags=os.XATTR_CREATE)
        errors = []
        for name, flags in (("user.test", os.XATTR_CREATE), ("user.absent", os.XATTR_REPLACE)):
            try:
                os.setxattr(p, name, b"x", flags=flags)
            except OSError as error:
                errors.append(error.errno)
            else:
                errors.append(0)
        return [os.getxattr(p, "user.test").hex(), errors]

    def mapped():
        p = root / "mapped"
        p.write_bytes(b"A" * 8192)
        with p.open("r+b") as stream:
            with mmap.mmap(stream.fileno(), 8192) as view:
                view[4093:4100] = b"crossed"
                view.flush()
            os.fsync(stream.fileno())
        return p.read_bytes()[4090:4103].decode()

    def sparse():
        p = root / "sparse"
        with p.open("w+b") as stream:
            stream.seek(65539)
            stream.write(b"Z")
            stream.truncate(131079)
        data = p.read_bytes()
        return [len(data), data[:65539] == b"\0" * 65539,
                data[65539] == 90, data[65540:] == b"\0" * (131079 - 65540)]

    def timestamp():
        p = root / "basic"
        os.utime(p, ns=(123456789123456789, 234567890987654321))
        return p.stat().st_mtime_ns

    def blocks():
        p = root / "basic"
        return p.stat().st_blocks > 0

    def open_unlink():
        p = root / "unlinked"
        with p.open("w+b") as stream:
            stream.write(b"open")
            stream.flush()
            p.unlink()
            stream.seek(0)
            result = [stream.read().decode(), os.fstat(stream.fileno()).st_nlink,
                      any(x.name.startswith(".fuse_hidden") for x in root.iterdir())]
        return result

    def long_name():
        p = root / ("n" * 116)
        p.write_bytes(b"x")
        return p.read_bytes().decode()

    def rename_dir():
        (root / "dir-a").mkdir()
        (root / "dir-b").mkdir()
        (root / "dir-a/child").mkdir()
        (root / "dir-a/child/value").write_bytes(b"moved")
        os.rename(root / "dir-a/child", root / "dir-b/child")
        return [(root / "dir-b/child/value").read_text(),
                (root / "dir-b/child/..").stat().st_ino == (root / "dir-b").stat().st_ino]

    def errors():
        observed = []
        for fn in (lambda: (root / "absent").open("rb"),
                   lambda: (root / "basic/child").stat(),
                   lambda: root.rmdir(),
                   lambda: os.open(root / "basic", os.O_CREAT | os.O_EXCL | os.O_WRONLY)):
            try:
                result = fn()
                if isinstance(result, int):
                    os.close(result)
            except OSError as error:
                observed.append(error.errno)
            else:
                observed.append(0)
        return observed

    def directory_sync():
        fd = os.open(root, os.O_DIRECTORY | os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        return True

    def allocation():
        fd = os.open(root / "allocated", os.O_CREAT | os.O_RDWR, 0o600)
        try:
            os.posix_fallocate(fd, 0, 65536)
            return os.fstat(fd).st_size
        finally:
            os.close(fd)

    for name, fn in (("links", basic), ("binary_xattr_flags", xattr),
                     ("shared_mmap", mapped), ("sparse_zero_fill", sparse),
                     ("timestamp_nanoseconds", timestamp), ("allocated_blocks", blocks),
                     ("open_unlink_visibility", open_unlink), ("name_116_bytes", long_name),
                     ("directory_rename_parent", rename_dir), ("errno", errors),
                     ("directory_fsync", directory_sync), ("posix_fallocate", allocation)):
        probe(name, fn)
    return results


def concurrent(root, worker):
    folder = root / f"client-{worker}"
    folder.mkdir()
    for index in range(100):
        p = folder / f"file-{index}"
        data = payload(worker * 1000 + index)[:8193]
        with p.open("wb") as stream:
            stream.write(data)
            os.fsync(stream.fileno())
        os.setxattr(p, "user.client", str(worker).encode())
        renamed = p.with_suffix(".renamed")
        p.rename(renamed)
        assert renamed.read_bytes() == data
        assert os.getxattr(renamed, "user.client") == str(worker).encode()
        os.link(renamed, p)
        renamed.unlink()
        assert p.read_bytes() == data
        if index % 2:
            p.unlink()
    assert len(list(folder.iterdir())) == 50
    return {"files_created": 100, "files_retained": 50}


def crash_writer(root, manifest):
    with manifest.open("a", buffering=1) as ack:
        for index in range(1000):
            tmp = root / "pending"
            data = payload(index)
            with tmp.open("wb") as stream:
                stream.write(data)
                os.fsync(stream.fileno())
            tmp.rename(root / f"durable-{index}")
            fd = os.open(root, os.O_DIRECTORY | os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            ack.write(json.dumps({"index": index, "sha256": hashlib.sha256(data).hexdigest()}) + "\n")
            os.fsync(ack.fileno())
    return True


def crash_verify(root, manifest):
    entries = [json.loads(line) for line in manifest.read_text().splitlines()]
    assert len(entries) >= 10, len(entries)
    for entry in entries:
        data = (root / f"durable-{entry['index']}").read_bytes()
        assert hashlib.sha256(data).hexdigest() == entry["sha256"], entry
    return {"acknowledged_files_verified": len(entries)}


class Suite:
    def __init__(self, args, report):
        self.args, self.root = args, report
        self.image, self.mountpoint = report / "disk.img", report / "mount"
        self.mountpoint.mkdir()
        self.process, self.mountlog = None, None
        self.results = []
        self.binary = str(Path(args.binary).resolve())

    def command(self, name, command, timeout=120):
        start = time.monotonic()
        record = {"name": name, "command": [str(x) for x in command]}
        try:
            with (self.root / (name + ".log")).open("wb") as log:
                result = subprocess.run(record["command"], stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            record.update(status="pass" if result.returncode == 0 else "fail", returncode=result.returncode)
        except subprocess.TimeoutExpired:
            record.update(status="fail", error="timeout")
        record["seconds"] = round(time.monotonic() - start, 3)
        self.results.append(record)
        print(f"{record['status'].upper()} {name} ({record['seconds']}s)", flush=True)
        return record["status"] == "pass"

    def worker(self, name, kind, path, *extra):
        return self.command(name, [sys.executable, __file__, "--worker", kind,
                                  "--directory", path, *extra])

    def mount(self, number):
        self.mountlog = (self.root / f"mount-{number}.log").open("wb")
        self.process = subprocess.Popen([self.binary, "--image", str(self.image), str(self.mountpoint)],
                                        stdout=self.mountlog, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 20
        while not os.path.ismount(self.mountpoint):
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("mount failed; see mount log")
            time.sleep(.05)

    def unmount(self, crash=False):
        if self.process is None:
            return
        if crash:
            self.process.kill()
            self.process.wait(timeout=10)
        result = subprocess.run(["fusermount3", "-uz" if crash else "-u", str(self.mountpoint)],
                                capture_output=True, timeout=10)
        if result.returncode:
            raise RuntimeError(result.stderr.decode())
        if not crash:
            if self.process.wait(timeout=15) != 0:
                raise RuntimeError("frontend failed on unmount")
        self.process = None
        self.mountlog.close()
        self.mountlog = None

    def fio(self, label, directory):
        metrics = []
        for repeat in range(self.args.repetitions):
            for name, rw, bs, size, sync in (
                    ("seqwrite", "write", "128k", "32m", False),
                    ("randwrite", "randwrite", "4k", "16m", False),
                    ("randread", "randread", "4k", "16m", False),
                    ("fsyncwrite", "write", "4k", "4m", True)):
                job = f"{label}-fio-{name}-{repeat}"
                output = self.root / (job + ".json")
                filename = directory / ("randwrite" if name == "randread" else name)
                command = ["fio", f"--name={name}", f"--filename={filename}", f"--rw={rw}",
                           f"--bs={bs}", f"--size={size}", "--ioengine=sync", "--iodepth=1",
                           "--numjobs=1", "--direct=0", "--thread=1", "--randseed=731",
                           "--fallocate=none", "--invalidate=1", "--fsync_on_close=1", "--eta=never",
                           "--group_reporting=1", "--output-format=json", f"--output={output}"]
                if rw != "randread":
                    command += ["--verify=crc32c", "--do_verify=1", "--verify_fatal=1"]
                if sync:
                    command += ["--fsync=1"]
                if self.command(job, command):
                    data = json.loads(output.read_text())["jobs"][0]
                    if data["error"]:
                        self.results[-1].update(status="fail", error=data["error"])
                    direction = "read" if rw == "randread" else "write"
                    io = data[direction]
                    metrics.append({"workload": name, "repeat": repeat,
                                    "MiB_per_s": io["bw_bytes"] / 1048576,
                                    "iops": io["iops"], "clat_mean_us": io["clat_ns"]["mean"] / 1000,
                                    "clat_p99_us": io["clat_ns"].get("percentile", {}).get("99.000000", 0) / 1000,
                                    "fsync_mean_us": data.get("sync", {}).get("lat_ns", {}).get("mean", 0) / 1000})
        return metrics

    def workload(self, label, directory):
        compat = directory / "compat"
        compat.mkdir()
        self.worker(label + "-compat", "compat", compat)
        for seed in (731, 20261112):
            name = f"{label}-fsx-{seed}"
            logs = self.root / name
            logs.mkdir()
            # This is the basic data/mmap/truncate profile. Unsupported extent
            # operations are explicitly disabled for BOTH filesystems.
            self.command(name, [self.args.fsx, "-N", str(self.args.fsx_operations),
                                "-S", str(seed), "-l", "1048576", "-o", "65536", "-X", "--record-ops",
                                "-F", "-H", "-z", "-C", "-I", "-J", "-B", "-E", "-u", "-a", "-T", "-K", "-0",
                                "-P", str(logs), str(directory / f"fsx-{seed}")], timeout=180)
        clients = directory / "clients"
        clients.mkdir()
        logs, processes = [], []
        start = time.monotonic()
        try:
            for client in range(4):
                log = (self.root / f"{label}-client-{client}.log").open("wb")
                logs.append(log)
                processes.append(subprocess.Popen([sys.executable, __file__, "--worker", "concurrent",
                                                   "--directory", str(clients), "--client", str(client)],
                                                  stdout=log, stderr=subprocess.STDOUT))
            codes = [p.wait(timeout=max(1, 120 - (time.monotonic() - start))) for p in processes]
            self.results.append({"name": label + "-concurrent", "status": "pass" if not any(codes) else "fail",
                                 "returncodes": codes, "seconds": time.monotonic() - start,
                                 "clients": 4, "files_per_client": 100})
        except subprocess.TimeoutExpired:
            self.results.append({"name": label + "-concurrent", "status": "fail", "error": "timeout"})
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
            for log in logs:
                log.close()
        return self.fio(label, directory)

    def crash(self, round_number):
        if self.process is None:
            self.mount(f"crash-{round_number}-writer")
        directory = self.mountpoint / f"crash-{round_number}"
        directory.mkdir()
        manifest = self.root / f"durable-manifest-{round_number}.jsonl"
        with (self.root / f"crash-writer-{round_number}.log").open("wb") as log:
            writer = subprocess.Popen([sys.executable, __file__, "--worker", "crash-write",
                                       "--directory", str(directory), "--manifest", str(manifest)],
                                      stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 45
                while not manifest.exists() or len(manifest.read_text().splitlines()) < 12:
                    if writer.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("crash writer did not acknowledge 12 files")
                    time.sleep(.01)
                if writer.poll() is not None:
                    raise RuntimeError("writer finished before crash injection")
                self.unmount(crash=True)
                writer.wait(timeout=15)
                self.results.append({"name": f"crash-injection-{round_number}", "status": "pass",
                                     "writer_active_before_kill": True,
                                     "acknowledged_files": len(manifest.read_text().splitlines())})
            finally:
                if writer.poll() is None:
                    writer.kill()
                    writer.wait(timeout=10)
        for cycle in range(3):
            self.mount(f"crash-{round_number}-recovery-{cycle}")
            self.worker(f"recovery-{round_number}-{cycle}", "crash-verify", directory, "--manifest", manifest)
            self.worker(f"stress-persist-{round_number}-{cycle}", "stress-verify", self.mountpoint / "clients")
            self.unmount()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/fuse/nvfuse-fuse")
    parser.add_argument("--fsx", default="fsx")
    parser.add_argument("--fsx-operations", type=int, default=2000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--crash-rounds", type=int, default=3)
    parser.add_argument("--report-dir", type=Path)
    parser.add_argument("--worker", choices=("compat", "concurrent", "crash-write", "crash-verify", "stress-verify"))
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--client", type=int, default=0)
    args = parser.parse_args()
    if args.worker:
        if args.worker == "compat":
            result = compatibility(args.directory)
        elif args.worker == "concurrent":
            result = concurrent(args.directory, args.client)
        elif args.worker == "crash-write":
            result = crash_writer(args.directory, args.manifest)
        elif args.worker == "crash-verify":
            result = crash_verify(args.directory, args.manifest)
        else:
            for client in range(4):
                folder = args.directory / f"client-{client}"
                assert {p.name for p in folder.iterdir()} == {f"file-{n}" for n in range(0, 100, 2)}
                for index in range(0, 100, 2):
                    p = folder / f"file-{index}"
                    assert p.read_bytes() == payload(client * 1000 + index)[:8193]
                    assert os.getxattr(p, "user.client") == str(client).encode()
            result = {"verified_files": 200}
        print(json.dumps(result, indent=2), flush=True)
        return
    if sys.platform != "linux" or not Path("/dev/fuse").exists():
        parser.error("Linux with /dev/fuse is required")
    for tool in (args.fsx, "fio", "fusermount3", "findmnt"):
        if not shutil.which(tool):
            parser.error(f"required reference tool missing: {tool}")
    if args.repetitions < 1 or args.fsx_operations < 1 or args.crash_rounds < 1:
        parser.error("operation and repetition counts must be positive")
    root = (args.report_dir or Path("build/test-results") /
            datetime.datetime.now().strftime("fuse-extended-%Y%m%d-%H%M%S-%f")).resolve()
    root.mkdir(parents=True, exist_ok=False)
    suite = Suite(args, root)
    reference = root / "reference"
    reference.mkdir()
    report = {"status": "running", "uname": list(os.uname()), "results": suite.results,
              "reference_mount": json.loads(subprocess.check_output(["findmnt", "-J", "-T", str(reference)])),
              "fio_version": subprocess.check_output(["fio", "--version"]).decode().strip(),
              "libfuse_version": subprocess.check_output(["pkg-config", "--modversion", "fuse3"]).decode().strip(),
              "fsx_binary": str(Path(shutil.which(args.fsx)).resolve()),
              "fsx_sha256": hashlib.sha256(Path(shutil.which(args.fsx)).read_bytes()).hexdigest(),
              "repetitions": args.repetitions, "fsx_operations_per_seed": args.fsx_operations,
              "crash_rounds": args.crash_rounds}
    fsx_source = Path(report["fsx_binary"]).parent.parent
    provenance = subprocess.run(["git", "-C", str(fsx_source), "rev-parse", "HEAD"], capture_output=True)
    if provenance.returncode == 0:
        report["fsx_source_commit"] = provenance.stdout.decode().strip()
    try:
        if not suite.command("mkfs", [suite.binary, "--mkfs", suite.image, "--size-mib", "512"]):
            raise RuntimeError("mkfs failed")
        report["benchmark"] = {"reference": suite.workload("reference", reference)}
        suite.mount(0)
        report["benchmark"]["nvfuse"] = suite.workload("nvfuse", suite.mountpoint)
        baseline = json.loads((root / "reference-compat.log").read_text())
        actual = json.loads((root / "nvfuse-compat.log").read_text())
        expected_reference = {
            "links": {"value": ["hello", 2, True]},
            "binary_xattr_flags": {"value": ["00ff", [errno.EEXIST, errno.ENODATA]]},
            "shared_mmap": {"value": "AAAcrossedAAA"},
            "sparse_zero_fill": {"value": [131079, True, True, True]},
            "timestamp_nanoseconds": {"value": 234567890987654321},
            "allocated_blocks": {"value": True},
            "open_unlink_visibility": {"value": ["open", 0, False]},
            "name_116_bytes": {"value": "x"},
            "directory_rename_parent": {"value": ["moved", True]},
            "errno": {"value": [errno.ENOENT, errno.ENOTDIR, errno.ENOTEMPTY, errno.EEXIST]},
            "directory_fsync": {"value": True},
            "posix_fallocate": {"value": 65536}}
        if baseline != expected_reference:
            raise RuntimeError("reference filesystem failed compatibility expectations; comparison invalid")
        # Accept only these exact documented divergences. EIO or any other
        # unexpected result in a limited feature still fails the suite.
        known = {"timestamp_nanoseconds": {"value": 234567890000000000},
                 "allocated_blocks": {"value": False},
                 "open_unlink_visibility": {"value": ["open", 0, True]},
                 "name_116_bytes": {"errno": errno.ENAMETOOLONG}}
        report["compatibility"] = []
        for name, expected in baseline.items():
            status = ("pass" if actual[name] == expected else
                      "limitation" if name in known and actual[name] == known[name] else "fail")
            item = {"name": name, "status": status, "reference": expected, "nvfuse": actual[name]}
            report["compatibility"].append(item)
            print(f"{status.upper()} compatibility {name}", flush=True)
        for round_number in range(args.crash_rounds):
            suite.crash(round_number)
        report["benchmark_medians"] = {}
        for label, samples in report["benchmark"].items():
            report["benchmark_medians"][label] = {
                name: {metric: statistics.median(s[metric] for s in samples if s["workload"] == name)
                       for metric in ("MiB_per_s", "iops", "clat_mean_us", "clat_p99_us", "fsync_mean_us")}
                for name in sorted({s["workload"] for s in samples})}
        failed = any(x["status"] == "fail" for x in suite.results + report["compatibility"])
        report["status"] = "fail" if failed else "pass_with_limitations"
        if not failed:
            suite.image.unlink()
            shutil.rmtree(reference)
    except BaseException as error:
        report.update(status="fail", error=repr(error))
        raise
    finally:
        try:
            if suite.process is not None:
                suite.unmount(crash=True)
        finally:
            (root / "summary.json").write_text(json.dumps(report, indent=2))
            print(f"Results: {root}", flush=True)
    if report["status"] == "fail":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
