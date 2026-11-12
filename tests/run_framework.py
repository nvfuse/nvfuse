#!/usr/bin/env python3
"""Deterministic NVFUSE/SPDK tests; preserve inputs and logs on failure."""
import argparse
import datetime
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


class Suite:
    def __init__(self, root):
        self.root = root.resolve()
        self.root.mkdir(parents=True, exist_ok=False)
        self.results = []

    def command(self, name, argv, expected=0, marker=None, timeout=90, **kwargs):
        started = time.monotonic()
        log = self.root / (name + ".log")
        try:
            with log.open("wb") as output:
                result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                        timeout=timeout, **kwargs)
            text = log.read_text(errors="replace")
            if result.returncode != expected:
                raise RuntimeError(f"exit {result.returncode}, expected {expected}; see {log}")
            if marker and marker not in text:
                raise RuntimeError(f"missing marker {marker!r}; see {log}")
            self.record(name, "pass", time.monotonic() - started, command=argv, log=str(log))
        except Exception as error:
            self.record(name, "fail", time.monotonic() - started, error=str(error), command=argv, log=str(log))
            if log.exists():
                print(log.read_text(errors="replace")[-8000:], file=sys.stderr)
            raise

    def record(self, name, status, seconds=0, **details):
        self.results.append(dict(name=name, status=status, seconds=seconds, **details))
        print(f"{status.upper()} {name}", flush=True)
        self.save()

    def save(self):
        (self.root / "results.json").write_text(json.dumps(self.results, indent=2) + "\n")
        tree = ET.Element("testsuite", name="nvfuse", tests=str(len(self.results)),
                          failures=str(sum(r["status"] == "fail" for r in self.results)),
                          skipped=str(sum(r["status"] == "skip" for r in self.results)))
        for row in self.results:
            case = ET.SubElement(tree, "testcase", name=row["name"], time=str(row["seconds"]))
            if row["status"] == "fail":
                ET.SubElement(case, "failure", message=row.get("error", "failed"))
            if row["status"] == "skip":
                ET.SubElement(case, "skipped", message=row.get("reason", ""))
        ET.ElementTree(tree).write(self.root / "junit.xml", encoding="unicode", xml_declaration=True)


def local(suite, args):
    binary = str(Path(args.binary).resolve())
    images = suite.root / "images"
    images.mkdir()
    # Core block-I/O failures deliberately abort; avoid writing large core dumps.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for seed in args.seeds:
        data, meta = images / f"data-{seed}.img", images / f"meta-{seed}.img"
        def run(mode, image, expected=0, marker="PASS "):
            suite.command(f"local-{seed}-{mode}", [binary, mode, str(image), str(seed)],
                          expected=expected, marker=marker)
        run("data-create", data)
        run("data-verify", data)
        run("reject-meta", data)
        run("meta-create", meta)
        run("reject-full", meta)
        for iteration in range(5):
            suite.command(f"local-{seed}-metadata-remount-{iteration}",
                          [binary, "meta-verify", str(meta), str(seed)], marker="PASS meta-verify")
        stored = images / f"stored-{seed}.img"
        run("stored-create", stored)
        run("stored-verify", stored)
        allocated = meta.stat().st_blocks * 512
        full_allocated = stored.stat().st_blocks * 512
        if full_allocated - allocated < 24 * 1024 * 1024:
            raise AssertionError(f"payload storage was not avoided: meta={allocated}, full={full_allocated}")
        suite.record(f"local-{seed}-metadata-storage", "pass", allocated_bytes=allocated,
                     full_allocated_bytes=full_allocated,
                     logical_payload_bytes=32 * 1024 * 1024 + 137)
        run("meta-delete", meta)
        run("meta-final", meta)
    suite.command("local-queue-capacity-and-completions",
                  [binary, "queues", str(images / "queues.raw"), "1"], marker="PASS queues")
    for case, marker in (("abort-range", "I/O beyond image boundary"),
                         ("abort-short-read", "file backend I/O")):
        suite.command(f"local-{case}", [binary, case, str(images / f"{case}.raw"), "1"],
                      expected=-signal.SIGABRT, marker=marker)
    if not args.keep_images:
        shutil.rmtree(images)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("local", "spdk", "integration", "integration-faults", "integration-powercuts", "all"), default="local")
    parser.add_argument("--binary", default="build/local/framework_test")
    parser.add_argument("--seeds", type=lambda s: [int(v, 0) for v in s.split(",")], default=[1, 0xc0ffee])
    parser.add_argument("--report-dir", type=Path,
                        default=Path(os.environ.get("NVFUSE_TEST_REPORT_ROOT", "build/test-results")) /
                        datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    parser.add_argument("--vm-dir", type=Path, default=Path("build/vm/rocky9"))
    fault_cases = ("read-eio", "write-eio", "stall-resume", "stall-timeout", "hotremove")
    parser.add_argument("--fault-cases", nargs="+", choices=fault_cases, default=fault_cases)
    parser.add_argument("--keep-images", action="store_true")
    parser.add_argument("--near-full-faults", action="store_true", help="fill the test filesystem to under 8 MiB free before NVMe faults")
    args = parser.parse_args()
    if not args.seeds or any(not 0 < s <= 0xffffffff for s in args.seeds):
        parser.error("seeds must be nonzero 32-bit integers")
    suite = Suite(args.report_dir)
    (suite.root / "run.json").write_text(json.dumps(dict(profile=args.profile, seeds=args.seeds,
        platform=sys.platform, python=sys.version, fault_cases=args.fault_cases,
        started=datetime.datetime.now().isoformat()), indent=2))
    try:
        if args.profile in ("local", "all"):
            local(suite, args)
        if args.profile in ("spdk", "all"):
            from spdk.vm_suite import run as spdk
            spdk(suite, args)
        if args.profile in ("integration", "integration-faults", "integration-powercuts", "all"):
            from spdk.integration_suite import run as integration
            integration(suite, args)
        else:
            suite.record("nvfuse-latest-spdk-integration", "skip",
                         reason="select --profile integration or all to run NVFUSE on the QEMU NVMe")
    except Exception as error:
        if not suite.results or suite.results[-1]["status"] != "fail":
            suite.record("framework", "fail", error=str(error))
        print(str(error), file=sys.stderr)
        return 1
    finally:
        suite.save()
        print(f"Results: {suite.root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
