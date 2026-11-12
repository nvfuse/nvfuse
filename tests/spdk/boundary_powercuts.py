"""Kill the whole dedicated QEMU VM at a journal barrier on a full image."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def run(suite, ssh, vm, guest, boot, stop):
    qemu_img = shutil.which('qemu-img')
    if not qemu_img:
        raise RuntimeError('qemu-img is required to retain the faulted image')
    repo = Path(__file__).resolve().parents[2]
    checker = repo / 'build/local/metadata_check'
    suite.command('vm-powercut-build-checker', ['make', '-C', str(repo), '-f', 'Makefile.local', 'build/local/metadata_check'], timeout=60)
    for phase, outcome in [('payload-flush', 'old'), ('checkpoint-write', 'new')]:
        prefix = 'vm-powercut-' + phase
        suite.command(prefix + '-fill', ssh + ['bash ~/src/nvfuse-framework/guest.sh integration-powercut-prepare 1'],
                      marker='PASS limit create size', timeout=180)
        output = (suite.root / (prefix + '-fill.log')).read_text()
        reached = json.loads(next(s[6:] for s in output.splitlines() if s.startswith('LIMIT ')))['reached']
        if guest('sync').returncode:
            raise RuntimeError('failed to sync the disposable guest OS before the cut')
        log = suite.root / (prefix + '-kill.log')
        started = time.monotonic()
        with log.open('wb') as stream:
            process = subprocess.Popen(ssh + [f'bash ~/src/nvfuse-framework/guest.sh integration-powercut-run 1 {reached} {phase}'],
                                       stdout=stream, stderr=subprocess.STDOUT)
            try:
                until = time.monotonic() + 60
                while f'READY VM cut phase={phase}' not in log.read_text(errors='replace'):
                    if process.poll() is not None or time.monotonic() > until:
                        raise RuntimeError(f'VM cut barrier not reached; see {log}')
                    time.sleep(0.05)
                # SIGKILL intentionally skips guest shutdown, QEMU cleanup and
                # host-backend flush-on-close. Only this fixture's PID is used.
                pidfile = vm / 'qemu.pid'
                pid = int(pidfile.read_text())
                os.kill(pid, signal.SIGKILL)
                process.wait(timeout=15)
                pidfile.unlink(missing_ok=True)
                for name in ('monitor.sock', 'qmp.sock'):
                    (vm / name).unlink(missing_ok=True)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        snapshot = suite.root / (prefix + '-original.qcow2')
        subprocess.run([qemu_img, 'convert', '-f', 'raw', '-O', 'qcow2', str(vm / 'nvme.raw'), str(snapshot)],
                       check=True, timeout=90)
        suite.record(prefix + '-kill', 'pass', time.monotonic() - started,
                     phase=phase, expected_state=outcome, snapshot=str(snapshot),
                     persistence_model='VM SIGKILL; host kernel/page cache remains powered')
        boot()
        for repeat in range(2):
            suite.command(prefix + f'-recover-{repeat}', ssh + [
                f'bash ~/src/nvfuse-framework/guest.sh integration-powercut-check 1 {reached} {outcome}'],
                marker='PASS limit check-crash size', timeout=180)
        # After successful recovery/clean close, inspect the actual disk bytes
        # independently with the host checker while QEMU is stopped. A reused
        # NVMe may contain stale payload; metadata-only reads synthesize zeros.
        stop()
        suite.command(prefix + '-raw-metadata', [str(checker), str(vm / 'nvme.raw'), '--skip-payload'],
                      marker='CHECK ', timeout=90)
        boot()
