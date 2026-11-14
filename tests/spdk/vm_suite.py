"""Host orchestration for the isolated QEMU VM; no physical devices accepted."""
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import tarfile
import time

FAULT_CONFIG = '''[set-state]
event = "write_aio"
state = "1"
new_state = "2"

[inject-error]
event = "read_aio"
state = "2"
errno = "5"
once = "on"
immediately = "on"
'''


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(15)
        self.sock.connect(str(path))
        self.file = self.sock.makefile("rwb", buffering=0)
        self.file.readline()
        self.call("qmp_capabilities")

    def call(self, command, arguments=None):
        request = dict(execute=command)
        if arguments is not None:
            request["arguments"] = arguments
        self.file.write((json.dumps(request) + "\n").encode())
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP disconnected")
            result = json.loads(line)
            if "error" in result:
                raise RuntimeError(result["error"])
            if "return" in result:
                return result["return"]

    def close(self):
        self.file.close()
        self.sock.close()


def run(suite, args):
    vm = args.vm_dir.resolve()
    ssh = ["sh", str(vm / "ssh.sh")]
    source = Path(__file__).resolve().parent
    def guest(command, **kwargs):
        return subprocess.run(ssh + [command], capture_output=True, timeout=30, **kwargs)

    def boot():
        subprocess.run(["sh", str(source / "start_vm.sh"), str(vm)], check=True, timeout=15)
        until = time.monotonic() + 60
        while time.monotonic() < until:
            if guest("true").returncode == 0:
                return
            time.sleep(1)
        raise RuntimeError("VM did not become SSH-ready")

    def stop():
        if not (vm / "qemu.pid").exists():
            return
        try:
            os.kill(int((vm / "qemu.pid").read_text()), 0)
        except ProcessLookupError:
            (vm / "qemu.pid").unlink()
            return
        result = guest("sudo poweroff")
        if result.returncode not in (0, 255):
            raise RuntimeError(result.stderr.decode(errors="replace"))
        until = time.monotonic() + 30
        while (vm / "qemu.pid").exists() and time.monotonic() < until:
            time.sleep(0.5)
        if (vm / "qemu.pid").exists():
            raise RuntimeError("VM did not shut down; refusing to modify running disk configuration")

    def test(name, case, seed=1, marker=None):
        suite.command(name, ssh + [f"bash ~/src/nvfuse-framework/guest.sh {case} {seed}"],
                      marker=marker or f"PASS {case}", timeout=70)

    config = vm / "blkdebug.conf"
    previous = config.read_bytes() if config.exists() else b""
    try:
        # Reboot is deliberate: blkdebug rules apply at block-node creation.
        stop()
        config.write_text("")
        boot()
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            for name in ("Makefile", "io_suite.c", "guest.sh"):
                archive.add(source / name, arcname=name)
        suite.command("spdk-build-test-program", ssh + [
            "set -e; mkdir -p ~/src/nvfuse-framework; tar -xzf - -C ~/src/nvfuse-framework; "
            ". ~/.venvs/spdk/bin/activate; make -C ~/src/nvfuse-framework; "
            "test -x ~/src/nvfuse-framework/io_suite; echo 'PASS io_suite built'; "
            "git -C ~/src/spdk describe --tags --exact-match; git -C ~/src/spdk rev-parse HEAD"],
            input=data.getvalue(), marker="io_suite", timeout=120)
        for seed in args.seeds:
            test(f"spdk-matrix-seed-{seed}", "matrix", seed)
        test("spdk-capacity-boundaries", "boundaries")
        for iteration in range(10):
            test(f"spdk-reconnect-{iteration}", "boundaries")
        name = "spdk-stalled-io-timeout-and-resume"
        log = suite.root / (name + ".log")
        started = time.monotonic()
        qmp = QMP(vm / "qmp.sock")
        throttle = dict(device="nvme0", bps=0, bps_rd=0, bps_wr=0, iops=0, iops_rd=0, iops_wr=0)
        try:
            with log.open("wb") as output:
                process = subprocess.Popen(ssh + ["bash ~/src/nvfuse-framework/guest.sh stalled 1"],
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    until = time.monotonic() + 20
                    while "READY stalled" not in log.read_text(errors="replace"):
                        if process.poll() is not None or time.monotonic() > until:
                            raise RuntimeError("timeout workload did not become ready")
                        time.sleep(0.1)
                    qmp.call("block_set_io_throttle", dict(throttle, iops_rd=1))
                    barrier = guest("touch /tmp/nvfuse-stall-go")
                    if barrier.returncode:
                        raise RuntimeError("failed to release timeout workload barrier")
                    while "TIMEOUT observed" not in log.read_text(errors="replace"):
                        if process.poll() is not None or time.monotonic() > until:
                            raise RuntimeError("SPDK did not detect the injected timeout")
                        time.sleep(0.1)
                    qmp.call("block_set_io_throttle", throttle)
                    if process.wait(timeout=30) or "PASS stalled" not in log.read_text(errors="replace"):
                        raise RuntimeError(f"timeout recovery failed; see {log}")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
            suite.record(name, "pass", time.monotonic() - started, log=str(log))
        except Exception as error:
            suite.record(name, "fail", time.monotonic() - started, error=str(error), log=str(log))
            raise
        finally:
            qmp.call("block_set_io_throttle", throttle)
            qmp.close()
        stop()
        config.write_text(FAULT_CONFIG)
        boot()
        test("spdk-injected-eio-and-recovery", "injected", marker="PASS injected transient error recovery")

        name = "spdk-hotremove"
        log = suite.root / (name + ".log")
        started = time.monotonic()
        qmp = QMP(vm / "qmp.sock")
        try:
            with log.open("wb") as output:
                process = subprocess.Popen(ssh + ["bash ~/src/nvfuse-framework/guest.sh hotremove 1"],
                                           stdout=output, stderr=subprocess.STDOUT)
                try:
                    until = time.monotonic() + 20
                    while "READY hotremove" not in log.read_text(errors="replace"):
                        if process.poll() is not None or time.monotonic() > until:
                            raise RuntimeError("hotremove workload did not become ready")
                        time.sleep(0.1)
                    qmp.call("device_del", dict(id="nvfuse-test-nvme"))
                    code = process.wait(timeout=35)
                    if code or "PASS hotremove" not in log.read_text(errors="replace"):
                        raise RuntimeError(f"hotremove exit {code}; see {log}")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
            suite.record(name, "pass", time.monotonic() - started, log=str(log))
        except Exception as error:
            suite.record(name, "fail", time.monotonic() - started, error=str(error), log=str(log))
            raise
        finally:
            qmp.close()
        # A clean reboot clears one-shot fault rules and restores Linux ownership.
        stop()
        config.write_text("")
        boot()
        test("spdk-after-vm-restart", "boundaries")
    finally:
        try:
            if (vm / "qemu.pid").exists():
                result = guest("tar -czf - -C /home/dev nvfuse-spdk-logs spdk-framework-build.log")
                if result.returncode == 0:
                    (suite.root / "guest-logs.tar.gz").write_bytes(result.stdout)
                stop()
        finally:
            if not (vm / "qemu.pid").exists():
                config.write_bytes(previous)
