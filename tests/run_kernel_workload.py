#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Linux kernel untar/build/remove workload on a NVFUSE FUSE mount.

Results measure the compatibility frontend, not the primary API/SPDK reactor.
Image mode creates a private image. SPDK mode opens an existing volume without
formatting; the caller must exclusively reserve and bind the intended device.
Only private workload directories are modified on an existing volume.
Failed trees and images are retained; logs and summaries live outside the mount.
"""
import argparse
import concurrent.futures
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import resource
import signal
import stat
import subprocess
import tarfile
import time


def digest(stream):
    result = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        result.update(chunk)
    return result.hexdigest()


def archive_manifest(archive):
    """Validate the single-root source archive and hash its original contents."""
    result, roots = {}, set()
    with tarfile.open(archive, "r:*") as source:
        for member in source:
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts or not name.parts:
                raise ValueError(f"Unsafe archive path: {member.name}")
            roots.add(name.parts[0])
            if len(roots) != 1:
                raise ValueError("Expected one top-level kernel source directory")
            if len(name.parts) == 1:
                if not member.isdir():
                    raise ValueError("Archive root must be a directory")
                continue
            relative = str(PurePosixPath(*name.parts[1:]))
            if relative in result:
                raise ValueError(f"Duplicate archive member: {relative}")
            item = {"mode": member.mode & 0o777, "kind": "directory"}
            if member.isfile():
                item.update(kind="file", size=member.size,
                            sha256=digest(source.extractfile(member)))
            elif member.issym():
                target = posixpath.normpath(posixpath.join(posixpath.dirname(relative), member.linkname))
                if member.linkname.startswith("/") or target == ".." or target.startswith("../"):
                    raise ValueError(f"Escaping source symlink: {relative}")
                item.update(kind="symlink", target=member.linkname)
            elif not member.isdir():
                raise ValueError(f"Unsupported archive member type: {relative}")
            result[relative] = item
    for relative in result:
        for parent in PurePosixPath(relative).parents:
            if str(parent) != "." and result.get(str(parent), {}).get("kind") != "directory":
                raise ValueError(f"Non-directory archive ancestor: {relative}")
    if result.get("Makefile", {}).get("kind") != "file" or "kernel" not in result:
        raise ValueError("Archive is not a Linux kernel source tree")
    return result


def verify_tree(tree, manifest, exact):
    for relative, expected in manifest.items():
        path = tree / relative
        observed = path.lstat()
        kind = ("symlink" if stat.S_ISLNK(observed.st_mode) else
                "directory" if stat.S_ISDIR(observed.st_mode) else
                "file" if stat.S_ISREG(observed.st_mode) else "other")
        if kind != expected["kind"]:
            raise AssertionError(f"Type mismatch: {path}")
        if kind != "symlink" and stat.S_IMODE(observed.st_mode) != expected["mode"]:
            raise AssertionError(f"Mode mismatch: {path}")
        if kind == "symlink" and os.readlink(path) != expected["target"]:
            raise AssertionError(f"Symlink mismatch: {path}")
        if kind == "file":
            with path.open("rb") as stream:
                if observed.st_size != expected["size"] or digest(stream) != expected["sha256"]:
                    raise AssertionError(f"Content mismatch: {path}")
    if exact:
        names = set()
        for directory, dirs, files in os.walk(tree, followlinks=False):
            names.update(str((Path(directory) / name).relative_to(tree)) for name in dirs + files)
        if names != set(manifest):
            raise AssertionError(f"Directory enumeration mismatch: {tree}")
    return {"source_entries_verified": len(manifest), "exact_namespace": exact}


class Runner:
    def __init__(self, args):
        self.args = args
        self.root = args.report_dir.resolve()
        self.root.mkdir(parents=True, exist_ok=False)
        self.mountpoint = self.root / "mount"
        self.mountpoint.mkdir()
        self.image = self.root / "filesystem.img"
        self.process = self.mountlog = None
        self.results = {"status": "running", "scope": f"FUSE+{args.backend} kernel application workload",
                        "arguments": vars(args), "phases": [], "groups": [],
                        "durability": "clean unmount/remount after each mutation phase; no power-cut claim",
                        "reference": "optional existing host filesystem; no cache or CPU equivalence claim"}

    def save(self):
        (self.root / "summary.json").write_text(json.dumps(self.results, default=str, indent=2) + "\n")

    def command(self, label, argv, cwd=None):
        started = time.monotonic()
        with (self.root / (label + ".log")).open("wb") as output:
            process = subprocess.Popen(list(map(str, argv)), cwd=cwd, stdout=output,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                rc = process.wait(timeout=self.args.timeout)
            except BaseException:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise
        result = {"worker": label, "seconds": time.monotonic() - started, "returncode": rc}
        if rc:
            raise RuntimeError(f"{label} failed ({rc}); see {label}.log")
        return result

    def parallel(self, label, tasks):
        print(f"START {label}", flush=True)
        started, before = time.monotonic(), resource.getrusage(resource.RUSAGE_CHILDREN)
        frontend_before = self.frontend_cpu()
        record = {"phase": label, "status": "running", "workers": []}
        self.results["phases"].append(record)
        self.save()
        errors = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(tasks)) as pool:
            futures = {pool.submit(task): index for index, task in enumerate(tasks)}
            for future in concurrent.futures.as_completed(futures):
                try:
                    record["workers"].append({"index": futures[future], "result": future.result()})
                except Exception as error:
                    errors.append(repr(error))
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        record.update(seconds=time.monotonic() - started, status="fail" if errors else "pass",
                      child_user_seconds=after.ru_utime - before.ru_utime,
                      child_system_seconds=after.ru_stime - before.ru_stime)
        frontend_after = self.frontend_cpu()
        if frontend_before is not None and frontend_after is not None:
            record["frontend_cpu_seconds"] = frontend_after - frontend_before
        if errors:
            record["errors"] = errors
        self.save()
        print(f"{record['status'].upper()} {label}: {record['seconds']:.3f}s", flush=True)
        if errors:
            raise RuntimeError("; ".join(errors))

    def frontend_cpu(self):
        if self.process is None:
            return None
        try:
            fields = Path(f"/proc/{self.process.pid}/stat").read_text().rsplit(")", 1)[1].split()
            return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
        except (OSError, ValueError):
            return None

    def mount(self, label):
        if self.process is not None or os.path.ismount(self.mountpoint):
            raise RuntimeError("Mount already active")
        self.mountlog = (self.root / (label + "-mount.log")).open("wb")
        backend = (["--image", str(self.image)] if self.args.backend == "imagefile" else
                   ["--backend", "spdk", "--target", self.args.target, "--nsid", str(self.args.nsid)])
        self.process = subprocess.Popen([str(self.args.binary), *backend,
                                         str(self.mountpoint)], stdout=self.mountlog,
                                        stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 30
        while not os.path.ismount(self.mountpoint):
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("FUSE mount failed; inspect mount log")
            time.sleep(0.05)

    def unmount(self):
        if os.path.ismount(self.mountpoint):
            subprocess.run(["fusermount3", "-u", str(self.mountpoint)], check=True, timeout=30)
        if self.process is not None:
            if self.process.wait(timeout=60):
                raise RuntimeError("FUSE reported a clean-unmount failure")
            self.process = None
        if self.mountlog is not None:
            self.mountlog.close()
            self.mountlog = None

    def reopen(self, label, fuse, base):
        started = time.monotonic()
        if fuse:
            self.unmount()
            self.mount(label)
        else:
            fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
            try:
                libc = ctypes.CDLL(None, use_errno=True)
                if libc.syncfs(fd):
                    raise OSError(ctypes.get_errno(), "reference syncfs failed")
            finally:
                os.close(fd)
        self.results["phases"].append({"phase": label + "-persistence-boundary",
                                       "seconds": time.monotonic() - started, "status": "pass"})
        self.save()

    def group(self, label, base, parallelism, manifest, fuse):
        folder = base / label
        folder.mkdir()
        trees = [folder / f"tree-{index}" for index in range(parallelism)]
        for tree in trees:
            tree.mkdir()
        group = {"label": label, "parallelism": parallelism, "status": "running"}
        self.results["groups"].append(group)
        self.parallel(label + "-untar", [
            lambda i=i, tree=tree: self.command(f"{label}-untar-{i}",
                ["tar", "--extract", "--file", self.args.archive, "--strip-components=1",
                 "--no-same-owner", "--same-permissions", "--directory", tree])
            for i, tree in enumerate(trees)])
        self.reopen(label + "-after-untar", fuse, base)
        self.parallel(label + "-verify-untar", [
            lambda tree=tree: verify_tree(tree, manifest, True) for tree in trees])
        self.parallel(label + "-configure", [
            lambda i=i, tree=tree: self.command(f"{label}-configure-{i}",
                ["make", self.args.config], tree) for i, tree in enumerate(trees)])
        self.parallel(label + "-build", [
            lambda i=i, tree=tree: self.command(f"{label}-build-{i}",
                ["make", f"-j{self.args.make_jobs}", "vmlinux"], tree)
            for i, tree in enumerate(trees)])
        self.reopen(label + "-after-build", fuse, base)
        self.parallel(label + "-verify-build", [
            lambda tree=tree: self.verify_build(tree, manifest) for tree in trees])
        self.parallel(label + "-rm", [
            lambda i=i, tree=tree: self.command(f"{label}-rm-{i}", ["rm", "-rf", "--", tree])
            for i, tree in enumerate(trees)])
        if list(folder.iterdir()):
            raise AssertionError(f"rm left entries in {folder}")
        folder.rmdir()
        self.reopen(label + "-after-rm", fuse, base)
        if folder.exists() or folder.is_symlink():
            raise AssertionError(f"Deleted tree returned after persistence boundary: {folder}")
        group["status"] = "pass"
        self.save()

    @staticmethod
    def verify_build(tree, manifest):
        result = verify_tree(tree, manifest, False)
        with (tree / "vmlinux").open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                raise AssertionError(f"Invalid vmlinux: {tree}")
            stream.seek(0)
            result["vmlinux_sha256"] = digest(stream)
        return result

    def run(self):
        try:
            self.save()
            with self.args.archive.open("rb") as stream:
                self.results["archive_sha256"] = digest(stream)
            manifest = archive_manifest(self.args.archive)
            (self.root / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            self.results["source_entries"] = len(manifest)
            self.results["environment"] = {
                "uname": list(os.uname()), "cpu_count": os.cpu_count(),
                "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0]}
            if self.args.backend == "imagefile":
                self.command("format", [self.args.binary, "--mkfs", self.image,
                                         "--size-mib", self.args.image_mib])
            self.mount("initial")
            for parallelism in self.args.parallel:
                for repeat in range(self.args.rounds):
                    label = f"fuse-p{parallelism}-r{repeat}"
                    self.group(label, self.mountpoint, parallelism, manifest, True)
            self.unmount()
            if self.args.reference_root:
                base = self.args.reference_root / self.root.name
                base.mkdir(exist_ok=False)
                self.results["reference_mount"] = subprocess.check_output(
                    ["findmnt", "-J", "-T", str(base)], text=True)
                for parallelism in self.args.parallel:
                    for repeat in range(self.args.rounds):
                        self.group(f"reference-p{parallelism}-r{repeat}", base,
                                   parallelism, manifest, False)
                base.rmdir()
            self.results["status"] = "pass"
        except Exception as error:
            self.results.update(status="fail", error=repr(error))
        finally:
            try:
                self.unmount()
            except Exception as error:
                self.results.update(status="fail", cleanup_error=repr(error))
            self.save()
        print(f"{self.results['status'].upper()} Results: {self.root}", flush=True)
        return 0 if self.results["status"] == "pass" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=Path("build/fuse/nvfuse-fuse"))
    parser.add_argument("--backend", choices=["imagefile", "spdk"], default="imagefile")
    parser.add_argument("--target", help="Explicit SPDK PCI BDF; existing volume, never formatted")
    parser.add_argument("--nsid", type=int, default=1)
    parser.add_argument("--parallel", type=int, nargs="+", default=[1, 2])
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--make-jobs", type=int, default=2, help="Compiler jobs per tree")
    parser.add_argument("--config", choices=["tinyconfig", "defconfig"], default="tinyconfig")
    parser.add_argument("--image-mib", type=int, default=8192)
    parser.add_argument("--timeout", type=int, default=1800, help="Seconds per external command")
    parser.add_argument("--reference-root", type=Path, help="Existing host directory; never formatted")
    parser.add_argument("--report-dir", type=Path, default=Path("build/test-results") /
                        ("kernel-workload-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")))
    args = parser.parse_args()
    if min(args.parallel + [args.rounds, args.make_jobs, args.timeout]) < 1:
        parser.error("parallelism, rounds, jobs and timeout must be positive")
    if args.image_mib < 128 or args.image_mib % 128:
        parser.error("image size must be a positive multiple of 128 MiB")
    if (args.backend == "spdk") != bool(args.target) or args.nsid < 1:
        parser.error("SPDK requires --target; image mode does not accept --target")
    for name in ("archive", "binary", "reference_root"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve(strict=True))
    os.umask(0o022)
    return Runner(args).run()


if __name__ == "__main__":
    raise SystemExit(main())
