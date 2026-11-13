#!/usr/bin/env python3
"""Crash the real primary reactor on a caller-owned, disposable bdev_aio image."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--config', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--bdf', required=True)
    p.add_argument('--report-dir', type=Path, required=True)
    a = p.parse_args()
    image = a.image.resolve(strict=True)
    assert image.is_file() and not str(image).startswith('/dev/')
    config = json.loads(a.config.read_text())
    commands = [c for s in config['subsystems'] for c in s.get('config', [])]
    assert len(commands) == 1 and commands[0]['method'] == 'bdev_aio_create'
    params = commands[0]['params']
    assert Path(params['filename']).resolve() == image and params['name'] == os.environ['NVFUSE_SPDK_BDEV']
    root = a.report_dir.resolve(); root.mkdir(parents=True, exist_ok=False)
    report = {'status': 'running', 'image': str(image), 'steps': []}

    def run(name, mode, phase='', expected=0):
        cmd = [str(a.binary.resolve()), str(a.config.resolve()), a.bdf, mode]
        if phase:
            cmd.append(phase)
        with (root / (name + '.log')).open('wb') as log:
            r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        report['steps'].append({'name': name, 'command': cmd, 'returncode': r.returncode, 'expected': expected})
        (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        assert r.returncode == expected, f'{name}: see {root / (name + ".log")}'
        print('PASS', name, flush=True)

    run('format', 'format')
    baseline = root / 'baseline.img'
    shutil.copyfile(image, baseline)
    for phase, committed in [('ordered-data-flush', False), ('payload-flush', False),
                             ('commit-flush', True), ('checkpoint-write', True), ('retire-flush', True)]:
        shutil.copyfile(baseline, image)
        with image.open('r+b') as f:
            os.fsync(f.fileno())
        run('crash-' + phase, 'mutate', phase, 86)
        run('recover-' + phase, 'check-new' if committed else 'check-old')
        run('reopen-' + phase, 'check-new' if committed else 'check-old')
    shutil.copyfile(baseline, image)
    run('payload-io-fault', 'io-fault', expected=87)
    run('recover-io-fault', 'check-old')
    # These are separate processes: the reused inode is cold at allocation and
    # its initialized metadata and zero-filled partial block must survive reopen.
    run('reuse-seed', 'reuse-seed')
    run('reuse-create', 'reuse-create')
    run('reuse-check', 'reuse-check')
    baseline.unlink()
    report['status'] = 'pass'
    (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
