#!/usr/bin/env python3
"""Repeat API/FUSE functional regression with complete, per-run coverage reports."""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

INTERFACES = ("api", "fuse3")
REQUIRED = {
    "api-create": 0, "api-reopen": 0, "exclusive-open": 1,
    "mounted-write-0": 0, "mounted-verify-1": 0,
    "mounted-verify-2": 0, "api-read-fuse-data": 0,
}


def validate_matrix(path, backend):
    report = json.loads(Path(path).read_text())
    if report.get("status") != "pass" or report.get("backend") != backend:
        raise ValueError(f"invalid {backend} matrix status/identity")
    required = dict(REQUIRED)
    if backend != "imagefile":
        required["reject-format-without-force"] = 2
    if backend == "kernel":
        required.update({"reject-unformatted-reopen": 1, "kernel-to-image-reopen": 0})
    stages = report.get("stages", [])
    if len(stages) != len(required) or {s.get("name") for s in stages} != set(required):
        raise ValueError(f"incomplete or unexpected {backend} stage set")
    for name, code in required.items():
        matches = [stage for stage in stages if stage.get("name") == name]
        if len(matches) != 1 or matches[0].get("returncode") != code:
            raise ValueError(f"missing, duplicate or failed {backend} stage: {name}")
    return report


def validate_cleanup(path):
    report = json.loads(Path(path).read_text())
    keys = {"status", "driver", "hugepages", "expected_hugepages", "interrupts", "expected_interrupts", "spdk_to_kernel"}
    if (not keys.issubset(report) or report.get("status") != "pass" or report.get("driver") != "nvme" or
            report.get("hugepages") != report.get("expected_hugepages") or
            report.get("interrupts") != report.get("expected_interrupts") or
            report.get("spdk_to_kernel") != "pass"):
        raise ValueError("SPDK cleanup/driver handoff incomplete")
    return report


class Regression:
    def __init__(self, root):
        self.root = root
        self.stages = []

    def run(self, name, command, timeout=900, evidence=None):
        record = {"name": name, "command": [str(x) for x in command], "status": "running",
                  "log": str(self.root / (name + ".log"))}
        self.stages.append(record)
        start = time.monotonic()
        print(f"RUN {name}", flush=True)
        try:
            with (self.root / (name + ".log")).open("wb") as log:
                process = subprocess.Popen(record["command"], stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
                try:
                    code = process.wait(timeout=timeout)
                except (subprocess.TimeoutExpired, KeyboardInterrupt):
                    # Give filesystem workers and the SPDK shell EXIT trap time
                    # to unmount/detach and restore the driver before force kill.
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=45)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=10)
                        record["cleanup"] = "not_confirmed_after_force_kill"
                    raise
            record["returncode"] = code
            if code:
                raise RuntimeError(f"exit status {code}")
            if evidence:
                evidence()
            record["status"] = "pass"
        except KeyboardInterrupt:
            record.update(status="fail", error="interrupted")
            raise
        except Exception as error:
            record.update(status="fail", error=repr(error))
        finally:
            record["seconds"] = round(time.monotonic() - start, 3)
            print(f"{record['status'].upper()} {name}", flush=True)
        return record["status"] == "pass"


def main():
    def stop(signum, frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise KeyboardInterrupt(f"received signal {signum}")

    signal.signal(signal.SIGTERM, stop)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("portable", "full"), default="portable")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--spdk-dir", type=Path, default=Path.home() / "src/spdk")
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run inside Linux; macOS callers use scripts/test-fuse-qemu.py --backends")
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    repo = Path(__file__).resolve().parent.parent
    os.chdir(repo)
    root = (args.report_dir or repo / "build/test-results" /
            datetime.datetime.now(datetime.timezone.utc).strftime("backend-regression-%Y%m%d-%H%M%S-%f")).resolve()
    root.mkdir(parents=True, exist_ok=False)
    regression = Regression(root)
    backends = ["imagefile", "kernel"] + (["spdk"] if args.profile == "full" else [])
    report = {"status": "running", "profile": args.profile, "repeat": args.repeat,
              "selected_backends": backends, "stages": regression.stages, "matrix": [],
              "uname": list(os.uname()), "benchmarks": False,
              "coverage_scope": "six_combinations" if args.profile == "full" else "four_portable_combinations"}
    lock = None
    try:
        (repo / "build").mkdir(exist_ok=True)
        lock = (repo / "build/backend-regression.lock").open("a")
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if not Path("/dev/fuse").is_char_device():
            raise RuntimeError("/dev/fuse is required; selected coverage is never silently skipped")
        privileged = [] if os.geteuid() == 0 else ["sudo", "-n"]
        if privileged:
            subprocess.run(privileged + ["true"], check=True, timeout=10, capture_output=True)
        build = regression.run("build", ["make", "-f", "Makefile.fuse", "fuse",
                                         "build/fuse/backend_test", "build/fuse/backend_options_test",
                                         "build/fuse/fuse_adapter_test"], timeout=300)
        if build:
            regression.run("options", ["build/fuse/backend_options_test"])
            regression.run("callbacks", [sys.executable, "tests/run_fuse_adapter.py", "build/fuse/fuse_adapter_test"])
        if args.sanitize:
            regression.run("callbacks-sanitized", ["env", "UBSAN_OPTIONS=halt_on_error=1", "make", "-f", "Makefile.fuse",
                           "LOCAL_BUILD=build/fuse-asan", "LOCAL_CFLAGS=-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer",
                           "fuse-adapter-test"], timeout=300)
        spdk_build = False
        if args.profile == "full":
            spdk_build = regression.run("build-spdk", ["make", "-C", "examples/fuse3", "-f", "Makefile.spdk",
                                        f"SPDK_ROOT_DIR={args.spdk_dir.resolve()}", "all", "backend-test"], timeout=300)
        for iteration in range(1, args.repeat + 1):
            round_dir = root / f"round-{iteration}"
            round_dir.mkdir()
            for backend in backends:
                folder = round_dir / backend
                ready = spdk_build if backend == "spdk" else build
                cell = {"round": iteration, "backend": backend, "interfaces": list(INTERFACES),
                        "status": "not_run", "report_dir": str(folder)}
                report["matrix"].append(cell)
                if not ready:
                    cell["reason"] = "required build failed"
                    continue
                if backend == "spdk":
                    command = ["env", f"SPDK_ROOT_DIR={args.spdk_dir.resolve()}", "bash",
                               "tests/spdk/backend_matrix_guest.sh", folder]
                    summary = folder / "results/summary.json"
                else:
                    command = ([sys.executable] if backend == "imagefile" else privileged + [sys.executable])
                    command += ["tests/run_backend_matrix.py", "--backend", backend, "--report-dir", folder]
                    summary = folder / "summary.json"

                def check(summary=summary, backend=backend, folder=folder):
                    validate_matrix(summary, backend)
                    if backend == "spdk":
                        validate_cleanup(folder / "cleanup.json")

                passed = regression.run(f"round-{iteration}-{backend}", command, evidence=check)
                cell["status"] = "pass" if passed else "fail"
        if (any(s["status"] != "pass" for s in regression.stages) or
                any(cell["status"] != "pass" for cell in report["matrix"])):
            report["status"] = "fail"
        else:
            report["status"] = "pass"
        for backend in backends:
            complete = all(c["status"] == "pass" for c in report["matrix"] if c["backend"] == backend)
            for interface in INTERFACES:
                print(f"{'PASS' if complete else 'FAIL'} {interface} + {backend} ({args.repeat} rounds)", flush=True)
    except BaseException as error:
        report.update(status="fail", error=repr(error))
        if isinstance(error, KeyboardInterrupt):
            report["interrupted"] = True
    finally:
        if lock:
            lock.close()
        (root / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"Results: {root}", flush=True)
    if report["status"] != "pass":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
