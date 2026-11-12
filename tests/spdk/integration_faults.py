"""Filesystem-level fault injection. Expected aborts do not imply crash recovery."""
import re
import json
import shutil
import subprocess
import time

from .vm_suite import QMP


def run(suite, ssh, vm, config, guest, boot, stop, cases):
    def command(name, case, marker, lba=0):
        suite.command(name, ssh + [f"bash ~/src/nvfuse-framework/guest.sh integration-fault 1 {case} {lba}"],
                      marker=marker, timeout=75)

    def prepare(name):
        suite.command(name, ssh + ["bash ~/src/nvfuse-framework/guest.sh integration-data-prepare 1"],
                      marker="PASS integration data-create", timeout=120)

    qemu_img = shutil.which("qemu-img")
    if not qemu_img:
        raise RuntimeError("qemu-img is required to preserve faulted images")
    for case in cases:
        prefix = f"integration-fault-{case}"
        prepare(prefix + "-prepare")
        command(prefix + "-locate", "locate", "LOCATED payload lba=")
        located = (suite.root / (prefix + "-locate.log")).read_text()
        lba = int(re.search(r"LOCATED payload lba=(\d+)", located)[1])
        if lba <= 0 or lba >= 256 * 1024 * 1024 // 512:
            raise AssertionError(f"unexpected payload LBA {lba}")
        stop()
        # A sector-specific rule cannot fire on the kernel probe or mount:
        # this is an existing file's first payload block, discovered above.
        if case.endswith("eio"):
            event = "write_aio" if case == "write-eio" else "read_aio"
            config.write_text(f'''[inject-error]
event = "{event}"
sector = "{lba}"
errno = "5"
once = "on"
immediately = "on"
''')
        else:
            config.write_text("")
        (suite.root / (prefix + "-blkdebug.conf")).write_bytes(config.read_bytes())
        boot()
        log = suite.root / (prefix + ".log")
        qmp = QMP(vm / "qmp.sock")
        throttle = dict(device="nvme0", bps=0, bps_rd=0, bps_wr=0, iops=0, iops_rd=0, iops_wr=0)
        started = time.monotonic()
        try:
            with log.open("wb") as output:
                process = subprocess.Popen(ssh + [
                    f"bash ~/src/nvfuse-framework/guest.sh integration-fault 1 {case} {lba}"],
                    stdout=output, stderr=subprocess.STDOUT)
                try:
                    def wait_for(marker, seconds=25):
                        until = time.monotonic() + seconds
                        while marker not in log.read_text(errors="replace"):
                            if process.poll() is not None or time.monotonic() > until:
                                raise RuntimeError(f"missing {marker!r}; see {log}")
                            time.sleep(0.05)

                    wait_for(f"READY fault {case}")
                    throttled = case.startswith("stall-") or case == "hotremove"
                    if throttled:
                        # QEMU may pass the first request before accounting its
                        # budget. Require a second submitted 4 KiB read below.
                        qmp.call("block_set_io_throttle", dict(throttle, bps_rd=1))
                    result = guest("touch /tmp/nvfuse-fault-go")
                    if result.returncode:
                        raise RuntimeError("failed to release filesystem I/O barrier")
                    wait_for(f"SUBMITTED fault {case}")
                    if throttled:
                        wait_for(f"SUBMITTED fault {case} ordinal=2")
                        time.sleep(0.3)
                        if process.poll() is not None:
                            raise RuntimeError("read was not held pending by QEMU")
                    if case == "stall-resume":
                        qmp.call("block_set_io_throttle", throttle)
                    elif case == "stall-timeout":
                        wait_for("NVFUSE SPDK: I/O timeout after 10 seconds", seconds=20)
                        # Let the EXIT trap rebind/probe the Linux NVMe driver.
                        qmp.call("block_set_io_throttle", throttle)
                    elif case == "hotremove":
                        before = qmp.call("query-pci")
                        if "nvfuse-test-nvme" not in json.dumps(before):
                            raise RuntimeError("target NVMe is absent before removal")
                        qmp.call("device_del", dict(id="nvfuse-test-nvme"))
                        wait_for("NVFUSE SPDK:", seconds=20)
                        qmp.call("block_set_io_throttle", throttle)
                    if process.wait(timeout=35):
                        raise RuntimeError(f"fault workload failed; see {log}")
                    if case == "hotremove":
                        until = time.monotonic() + 10
                        while True:
                            after = qmp.call("query-pci")
                            (suite.root / (prefix + "-pci.json")).write_text(json.dumps(
                                dict(before=before, after=after), indent=2) + "\n")
                            if "nvfuse-test-nvme" not in json.dumps(after):
                                break
                            if time.monotonic() > until:
                                raise RuntimeError("QEMU accepted device_del but PCI device is still present")
                            time.sleep(0.1)
                    text = log.read_text(errors="replace")
                    marker = ("PASS fault stall-resume full payload compared" if case == "stall-resume"
                              else f"PASS fault {case} expected SIGABRT")
                    if marker not in text:
                        raise RuntimeError(f"missing fault outcome; see {log}")
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
            suite.record(prefix, "pass", time.monotonic() - started, log=str(log),
                         payload_lba=lba, throttle_read_bytes_per_second=1 if throttled else 0,
                         outcome="clean resume" if case == "stall-resume" else "fail-fast SIGABRT")
        except Exception as error:
            suite.record(prefix, "fail", time.monotonic() - started, log=str(log), error=str(error))
            print(log.read_text(errors="replace")[-6000:], flush=True)
            raise
        finally:
            try:
                # Throttle belongs to the retained block backend even on unplug.
                qmp.call("block_set_io_throttle", throttle)
            finally:
                qmp.close()
        stop()
        if case != "stall-resume":
            # Convert only while stopped; keep compact evidence before any mkfs.
            image = suite.root / (prefix + ".qcow2")
            suite.command(prefix + "-preserve-image", [qemu_img, "convert", "-f", "raw", "-O", "qcow2",
                          str(vm / "nvme.raw"), str(image)], timeout=120)
        config.write_text("")
        boot()
        if case == "stall-resume":
            suite.command(prefix + "-reboot-verify", ssh + [
                "bash ~/src/nvfuse-framework/guest.sh integration-data-check 1"],
                marker="PASS integration data-verify", timeout=120)
        else:
            command(prefix + "-unclean-rejected", "unclean", "PASS unclean mounted image rejected")
    # Demonstrate device/environment reuse, without claiming old-image recovery.
    prepare("integration-fault-reinitialize")
    suite.command("integration-fault-reinitialized-data-verify", ssh + [
        "bash ~/src/nvfuse-framework/guest.sh integration-data-check 1"],
        marker="PASS integration data-verify", timeout=120)
    suite.record("integration-automatic-crash-recovery", "skip",
                 reason="core aborts on block I/O failure and rejects mounted images; automatic recovery is not implemented")
