#!/usr/bin/env python3
"""Six sequential/random fio profiles on an existing filesystem; no formatting.

Writes and overwrites four benchmark files. For the API/SPDK engine supply
NVFUSE_SPDK_JSON/BDF/BDEV and NVFUSE_FORMAT=0 in the environment.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

PROFILES = [('seq-write', '256k', 'rw', 0), ('seq-read', '256k', 'rw', 100),
            ('seq-mix75', '256k', 'rw', 75), ('rand-write', '4k', 'randrw', 0),
            ('rand-read', '4k', 'randrw', 100), ('rand-mix75', '4k', 'randrw', 75)]


def main():
    def stop(signum, frame):
        raise InterruptedError(f'received signal {signum}')

    signal.signal(signal.SIGTERM, stop)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fio', type=Path, required=True)
    parser.add_argument('--engine', required=True, help='libaio or absolute nvfuse_spdk.so path')
    parser.add_argument('--directory', required=True, help='mounted directory, or / inside the API filesystem')
    parser.add_argument('--runtime', type=int, default=10)
    parser.add_argument('--size', default='20g', help='size per job')
    parser.add_argument('--prepare', action='store_true', help='fully populate all files before timing')
    parser.add_argument('--report-dir', type=Path, required=True)
    args = parser.parse_args()
    if args.runtime <= 0:
        parser.error('runtime must be positive')
    if os.environ.get('NVFUSE_FORMAT') == '1':
        parser.error('format separately; NVFUSE_FORMAT must be 0 or unset')
    root = args.report_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {'status': 'running', 'engine': args.engine, 'runtime': args.runtime,
              'size_per_job': args.size, 'jobs': 4, 'iodepth': 32, 'direct': 1, 'steps': []}

    def save():
        (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(name, bs, rw, mix, timed=True):
        command = [str(args.fio.resolve(strict=True)), '--name=test',
                   '--directory=' + args.directory, '--filename_format=bench.$jobnum',
                   '--size=' + args.size, '--direct=1', '--bs=' + bs,
                   '--ioengine=' + args.engine, '--iodepth=32', '--thread=1', '--numjobs=4',
                   '--rw=' + rw, '--rwmixread=' + str(mix), '--group_reporting=1',
                   '--output-format=json', '--output=' + str(root / (name + '.json'))]
        if timed:
            command += ['--runtime=' + str(args.runtime), '--time_based=1']
        else:
            command += ['--end_fsync=1']
        step = {'name': name, 'status': 'running', 'command': command}
        report['steps'].append(step)
        save()
        started = time.monotonic()
        with (root / (name + '.log')).open('wb') as log:
            proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                step['returncode'] = proc.wait(timeout=args.runtime + 120 if timed else 1800)
            except BaseException:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                raise
        step['elapsed'] = time.monotonic() - started
        if step['returncode']:
            raise RuntimeError(f'{name} failed; see its runtime log')
        raw = (root / (name + '.json')).read_text()
        # fio can place informational warnings before its JSON. Preserve the
        # original output and record any prefix rather than discarding it.
        start = raw.find('{')
        if start < 0:
            raise RuntimeError(f'{name} produced no JSON')
        result = json.loads(raw[start:])
        if raw[:start].strip():
            step['fio_messages'] = raw[:start].strip()
        if any(j['error'] for j in result['jobs']):
            raise RuntimeError(f'{name} reported I/O errors')
        step['status'] = 'pass'
        save()
        print('PASS', name, flush=True)

    try:
        if args.prepare:
            run('prepare', '256k', 'write', 0, timed=False)
        for profile in PROFILES:
            run(*profile)
        report['status'] = 'pass'
    except BaseException as exc:
        report.update(status='fail', error=str(exc))
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
