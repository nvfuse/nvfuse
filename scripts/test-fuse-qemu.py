#!/usr/bin/env python3
"""Run Linux FUSE regression in the dedicated QEMU VM and collect this run only."""
import argparse
import datetime
import io
import json
from pathlib import Path
import shlex
import subprocess
import tarfile
import time


def main():
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm-dir", type=Path, default=repo / "build/vm/rocky9")
    parser.add_argument("--report-dir", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--extended", action="store_true", help="Explicitly add fsx/stress/fio; not part of functional regression")
    parser.add_argument("--backends", action="store_true", help="Full API/FUSE x image/kernel/SPDK functional regression")
    parser.add_argument("--repeat", type=int, default=1, help="Sequential full-matrix rounds (requires --backends)")
    parser.add_argument("--fsx", default="/home/dev/src/fs-reference/xfstests/ltp/fsx")
    args = parser.parse_args()
    if args.repeat < 1 or (args.repeat != 1 and not args.backends):
        parser.error("positive --repeat requires --backends")
    vm = args.vm_dir.resolve()
    if not (vm / "ssh.sh").is_file():
        parser.error("Provision the dedicated QEMU VM first; VM_DIRECTORY/ssh.sh is required")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    root = (args.report_dir or repo / "build/test-results" / ("fuse-qemu-" + stamp)).resolve()
    root.mkdir(parents=True, exist_ok=False)
    guest_root = "build/test-results/backend-regression-" + stamp
    summary_path = guest_root + ("/summary.json" if args.backends else "/smoke/summary.json")
    ssh = ["sh", str(vm / "ssh.sh")]
    report = {"status": "running", "guest_report_dir": guest_root, "repeat": args.repeat,
              "full_backend_regression": args.backends, "benchmarks_requested": args.extended}
    launched = False
    stdout, stderr = b"", b""
    try:
        if subprocess.run(ssh + ["true"], capture_output=True, timeout=10).returncode:
            subprocess.run(["sh", str(repo / "tests/spdk/start_vm.sh"), str(vm)], check=True, timeout=20)
            deadline = time.monotonic() + 60
            while subprocess.run(ssh + ["true"], capture_output=True, timeout=10).returncode:
                if time.monotonic() > deadline:
                    raise TimeoutError("QEMU guest did not become ready")
                time.sleep(1)
        data = io.BytesIO()
        excluded = {".git", "build", ".agents", ".codex", ".aws", "__pycache__"}
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            archive.add(repo, arcname="nvfuse", filter=lambda item:
                        None if excluded.intersection(Path(item.name).parts) else item)
        commands = (
            "set -e; mkdir -p ~/src/nvfuse-fuse-dev; "
            "tar -xzf - -C ~/src/nvfuse-fuse-dev --strip-components=1; "
            "cd ~/src/nvfuse-fuse-dev; uname -srmo; "
        )
        if args.backends:
            commands += ("python3 tests/run_backend_regression.py --profile full --report-dir " +
                         shlex.quote(guest_root) + " --repeat " + str(args.repeat))
            if args.sanitize:
                commands += " --sanitize"
        else:
            commands += ("make fuse fuse-adapter-test; python3 tests/run_fuse.py --report-dir " +
                         shlex.quote(guest_root + "/smoke"))
            if args.sanitize:
                commands += ('; UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.fuse '
                             'LOCAL_BUILD=build/fuse-asan '
                             'LOCAL_CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" '
                             'fuse-adapter-test')
        if args.extended:
            commands += ("; python3 tests/run_fuse_extended.py --fsx " + shlex.quote(args.fsx) +
                         " --report-dir " + shlex.quote(guest_root + "/extended"))
        launched = True
        try:
            result = subprocess.run(ssh + [commands], input=data.getvalue(), capture_output=True,
                                    timeout=1800 * args.repeat if args.backends else 900)
            stdout, stderr = result.stdout, result.stderr
            report["returncode"] = result.returncode
            report["status"] = "pass" if result.returncode == 0 else "fail"
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b"", error.stderr or b""
            report.update(status="fail", returncode=124, error="guest SSH command timed out; cleanup not confirmed")
    except Exception as error:
        report.update(status="fail", error=repr(error))
    finally:
        (root / "guest.log").write_bytes(stdout + stderr)
        if launched:
            try:
                collection = subprocess.run(ssh + [
                    "set -e; cd ~/src/nvfuse-fuse-dev; test -d " + shlex.quote(guest_root) + "; "
                    "find " + shlex.quote(guest_root) +
                    " \\( -name mount -o -name reference \\) -prune -o -type f "
                    "\\( -name '*.log' -o -name '*.json' -o -name '*.jsonl' "
                    "-o -name '*.fsxlog' -o -name '*.fsxops' -o -name '*.fsxgood' \\) "
                    "-print0 | tar --null -T - -czf -"
                ], capture_output=True, timeout=60)
                (root / "collection.log").write_bytes(collection.stderr)
                if collection.returncode:
                    raise RuntimeError("could not collect guest evidence")
                (root / "mounted-reports.tar.gz").write_bytes(collection.stdout)
                guest = subprocess.run(ssh + ["cd ~/src/nvfuse-fuse-dev; cat " + shlex.quote(summary_path)],
                                       capture_output=True, timeout=15)
                if guest.returncode:
                    raise RuntimeError("guest summary missing")
                parsed = json.loads(guest.stdout)
                (root / "guest-summary.json").write_bytes(guest.stdout)
                if parsed.get("status") != "pass":
                    report["status"] = "fail"
                report["coverage_scope"] = parsed.get("coverage_scope", "mounted_image_smoke")
                if args.extended:
                    extended = subprocess.run(ssh + ["cd ~/src/nvfuse-fuse-dev; cat " +
                                              shlex.quote(guest_root + "/extended/summary.json")],
                                              capture_output=True, timeout=15)
                    if extended.returncode or json.loads(extended.stdout).get("status") not in ("pass", "pass_with_limitations"):
                        raise RuntimeError("extended suite incomplete or failed")
            except Exception as error:
                report.update(status="fail", collection_error=repr(error))
        (root / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
        print(stdout.decode(errors="replace")[-6000:])
        if stderr:
            print(stderr.decode(errors="replace")[-2000:])
        print(f"{report['status'].upper()} Results: {root}")
    if report["status"] != "pass":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
