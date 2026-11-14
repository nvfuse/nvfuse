#!/usr/bin/env python3
"""Destructive primary API/SPDK fio regression on an explicitly prepared bdev.

This runner never binds PCI devices or changes hugepages. The caller prepares
SPDK JSON and device ownership; --format is required to acknowledge that the
selected NVFUSE filesystem will be replaced. QEMU fixture guards are separate.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import signal
import subprocess


def main():
    def stop(signum, frame):
        raise InterruptedError(f'received signal {signum}')

    signal.signal(signal.SIGTERM, stop)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fio', type=Path, required=True)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--bdf', required=True)
    parser.add_argument('--bdev', required=True)
    parser.add_argument('--format', action='store_true', required=True,
                        help='replace the entire selected NVFUSE filesystem')
    parser.add_argument('--size', default='64M', help='large-file range (default: 64M)')
    parser.add_argument('--report-dir', type=Path, required=True)
    args = parser.parse_args()
    fio, engine, config = (str(p.resolve(strict=True)) for p in
                            (args.fio, args.engine, args.config))
    root = args.report_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {'status': 'running', 'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'bdf': args.bdf, 'bdev': args.bdev, 'config': json.loads(Path(config).read_text()),
              'steps': []}

    def save():
        (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(name, filename, size, bs, depth, rw, verify=False, reopen=False, seconds=None, format=False, jobs=1):
        command = [fio, '--name=' + ('fio-multi' if jobs > 1 else name), '--ioengine=' + engine,
                   '--thread=1', '--numjobs=' + str(jobs), '--group_reporting=1', '--nrfiles=1', '--direct=1',
                   ('--filename_format=' if jobs > 1 else '--filename=') + filename, '--size=' + size, '--bs=' + bs,
                   '--iodepth=' + str(depth), '--rw=' + rw, '--output-format=json',
                   '--output=' + str(root / (name + '.json'))]
        if verify or reopen:
            command += ['--verify=crc32c', '--verify_fatal=1']
            command += ['--verify_only=1'] if reopen else ['--do_verify=1', '--end_fsync=1']
        if seconds:
            command += ['--time_based=1', '--runtime=' + str(seconds)]
        env = dict(os.environ, NVFUSE_SPDK_JSON=config, NVFUSE_SPDK_BDF=args.bdf,
                   NVFUSE_SPDK_BDEV=args.bdev, NVFUSE_FORMAT='1' if format else '0')
        step = {'name': name, 'command': command, 'format': format}
        report['steps'].append(step)
        save()
        with (root / (name + '.log')).open('wb') as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                step['returncode'] = process.wait(timeout=180)
            except BaseException:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                raise
        save()
        if step['returncode']:
            raise RuntimeError(f'{name} failed; see {root / (name + ".log")}')
        result = json.loads((root / (name + '.json')).read_text())
        if len(result['jobs']) != 1 or result['jobs'][0]['error']:
            raise RuntimeError(f'{name}: fio reported an I/O error')
        print('PASS', name, flush=True)

    try:
        run('smoke-crc', '/fio-data', '16M', '4k', 8, 'write', verify=True, format=True)
        run('boundary-64k-crc', '/fio-large', args.size, '64k', 8, 'write', verify=True)
        run('reopen-64k-crc', '/fio-large', args.size, '64k', 8, 'read', reopen=True)
        run('boundary-1m-crc', '/fio-meg', args.size, '1m', 1, 'write', verify=True)
        run('reopen-1m-crc', '/fio-meg', args.size, '1m', 1, 'read', reopen=True)
        run('sequential-read-1m', '/fio-large', args.size, '1m', 1, 'read', seconds=5)
        run('random-read-4k', '/fio-large', args.size, '4k', 32, 'randread', seconds=5)
        run('random-write-crc', '/fio-random', '64M', '4k', 32, 'randwrite', verify=True)
        multi = '/fio-multi.$jobnum.$filenum.$jobname'
        run('multi-256k-crc', multi, '32M', '256k', 32, 'write', verify=True, jobs=4)
        run('multi-reopen-crc', multi, '32M', '256k', 32, 'read', reopen=True, jobs=4)
        run('multi-randmix', multi, '32M', '4k', 32, 'randrw', seconds=3, jobs=4)
        report['status'] = 'pass'
    except BaseException as exc:
        report['status'] = 'fail'
        report['error'] = str(exc)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
