"""Run the NVFUSE filesystem tests through real SPDK on the dedicated QEMU NVMe."""
import io
import os
from pathlib import Path
import re
import subprocess
import tarfile
import time


def run(suite, args):
    vm = args.vm_dir.resolve()
    source = Path(__file__).resolve().parent
    repo = source.parent.parent
    ssh = ["sh", str(vm / "ssh.sh")]

    def guest(command):
        return subprocess.run(ssh + [command], capture_output=True, timeout=30)

    def boot():
        subprocess.run(["sh", str(source / "start_vm.sh"), str(vm)], check=True, timeout=15)
        until = time.monotonic() + 60
        while time.monotonic() < until:
            if guest("true").returncode == 0:
                return
            time.sleep(1)
        raise RuntimeError("VM did not become SSH-ready")

    def stop():
        pidfile = vm / "qemu.pid"
        if not pidfile.exists():
            return
        try:
            os.kill(int(pidfile.read_text()), 0)
        except ProcessLookupError:
            pidfile.unlink()
            return
        result = guest("sudo poweroff")
        if result.returncode not in (0, 255):
            raise RuntimeError(result.stderr.decode(errors="replace"))
        until = time.monotonic() + 30
        while pidfile.exists() and time.monotonic() < until:
            time.sleep(0.5)
        if pidfile.exists():
            raise RuntimeError("VM did not shut down; refusing to modify running disk configuration")

    config = vm / "blkdebug.conf"
    previous = config.read_bytes() if config.exists() else b""
    try:
        stop()
        config.write_text("")
        boot()
        data = io.BytesIO()
        excluded = {".git", "build", ".agents", ".codex", ".aws", "__pycache__"}

        def select(info):
            return None if excluded.intersection(Path(info.name).parts) else info

        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            archive.add(repo, arcname="nvfuse", filter=select)
        suite.command("integration-build", ssh + [
            "set -e; mkdir -p ~/src; tar -xzf - -C ~/src; "
            "mkdir -p ~/src/nvfuse-framework; "
            "cp ~/src/nvfuse/tests/spdk/guest.sh ~/src/nvfuse-framework/guest.sh; "
            ". ~/.venvs/spdk/bin/activate; cd ~/src/nvfuse/tests/spdk; "
            "make -f Makefile.integration; make -f Makefile.integration APP=integration_framework; "
            "test -x integration_test; test -x integration_framework; "
            "git -C ~/src/spdk describe --tags --exact-match; git -C ~/src/spdk rev-parse HEAD; "
            "echo 'PASS integration built'"], input=data.getvalue(),
            marker="PASS integration built", timeout=180)
        for seed in args.seeds:
            name = f"integration-seed-{seed}"
            suite.command(name, ssh + [f"bash ~/src/nvfuse-framework/guest.sh integration {seed}"],
                          marker="PASS integration suite", timeout=300)
            output = (suite.root / (name + ".log")).read_text()
            stages = re.findall(r"^PASS integration (\S+) seed=", output, re.M)
            counters = re.findall(r"SPDK_IO reads=(\d+) writes=(\d+) flushes=(\d+)", output)
            totals = [sum(int(row[i]) for row in counters) for i in range(3)]
            # 'reopen' closes/reopens three additional times in one process.
            if len(stages) != 16 or len(counters) != 19 or not all(totals):
                raise AssertionError(f"incomplete NVMe filesystem coverage: stages={stages}, I/O={totals}")
            suite.record(f"integration-coverage-{seed}", "pass", stages=stages,
                         nvme_reads=totals[0], nvme_writes=totals[1], nvme_flushes=totals[2])
        # Verify persisted payload and metadata across separate full VM boots.
        for mode in ("data", "meta"):
            for phase in ("prepare", "check"):
                if phase == "check":
                    stop()
                    boot()
                suite.command(f"integration-vm-restart-{mode}-{phase}", ssh + [
                    f"bash ~/src/nvfuse-framework/guest.sh integration-{mode}-{phase} {args.seeds[-1]}"],
                    marker=f"PASS integration {mode}-{'create' if phase == 'prepare' else 'verify'}",
                    timeout=120)
    finally:
        try:
            if (vm / "qemu.pid").exists():
                result = guest("tar -czf - -C /home/dev nvfuse-spdk-logs")
                if result.returncode == 0:
                    (suite.root / "integration-guest-logs.tar.gz").write_bytes(result.stdout)
                stop()
        finally:
            if not (vm / "qemu.pid").exists():
                config.write_bytes(previous)
