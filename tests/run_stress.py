#!/usr/bin/env python3
"""Mixed-operation oracle, maximum inode sweep, and full-image crash recovery."""
import argparse
import datetime
import json
import os
from pathlib import Path
import resource
import shutil
import sys
from run_framework import Suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build/local')
    parser.add_argument('--seeds', nargs='+', type=int, default=[1, 42, 0xc0ffee])
    parser.add_argument('--verify-every',type=int,default=1)
    parser.add_argument('--remount-every',type=int,default=100)
    parser.add_argument('--command-timeout',type=int,default=240)
    parser.add_argument('--rounds', type=int, default=3000)
    parser.add_argument('--sections', nargs='+', choices=['mixed', 'inodes', 'crash'], default=['mixed', 'inodes', 'crash'])
    parser.add_argument('--report-dir', type=Path, default=Path(os.environ.get('NVFUSE_TEST_REPORT_ROOT', 'build/test-results')) /
                        ('stress-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    args = parser.parse_args()
    if min(args.rounds,args.verify_every,args.remount_every,args.command_timeout)<1 or any(s <= 0 or s > 0xffffffff for s in args.seeds):
        parser.error('rounds must be positive and seeds must be nonzero uint32 values')
    suite = Suite(args.report_dir)
    build = Path(args.build_dir).resolve()
    images = suite.root / 'images'
    images.mkdir()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    (suite.root / 'run.json').write_text(json.dumps(vars(args), default=str, indent=2) + '\n')

    def command(name, argv, marker='PASS ', expected=0, timeout=240):
        suite.command(name, list(map(str, argv)), marker=marker, expected=expected, timeout=max(timeout,args.command_timeout))
        return (suite.root / (name + '.log')).read_text(errors='replace')

    def raw(name, image):
        output = command(name, [build / 'metadata_check', image], marker='CHECK ')
        return json.loads(next(line[6:] for line in output.splitlines() if line.startswith('CHECK ')))

    def operation(name, mode, image, kind, reached, op, *extra, expected=0):
        return command(name, [build / 'limit_test', mode, image, kind, 128, reached, op, *extra],
                       marker=None if expected else f'PASS limit {mode}', expected=expected)

    try:
        if 'inodes' in args.sections:
            command('inode-full-space', [build / 'inode_space_test', '--full'], marker='PASS inode space', timeout=300)
        if 'mixed' in args.sections:
            for seed in args.seeds:
                for meta in (0, 1):
                    name = f'mixed-{seed}-{meta}'
                    image = images / (name + '.img')
                    command(name, [build / 'mixed_model_test', image, seed, args.rounds, meta,args.verify_every,args.remount_every], marker='PASS mixed namespace')
                    raw(name + '-raw', image)
                    image.unlink()
        if 'crash' in args.sections:
            phases = [('prepare-flush', 1, 'old'), ('prepare-flush', 2, 'old'),
                      ('payload-write', 1, 'old'), ('payload-flush', 1, 'old'),
                      ('commit-flush', 1, 'new'), ('commit-flush', 2, 'new'),
                      ('checkpoint-write', 1, 'new'), ('checkpoint-flush', 1, 'new'),
                      ('retire-flush', 1, 'new'), ('retire-flush', 2, 'new')]
            for kind in ('files', 'dirs', 'size'):
                base = images / (kind + '-full.img')
                output = command(kind + '-fill', [build / 'limit_test', 'create', base, kind, 128])
                reached = json.loads(next(line[6:] for line in output.splitlines() if line.startswith('LIMIT ')))['reached']
                for op in (('create', 'delete', 'truncate') if kind == 'size' else ('create', 'delete')):
                    prefix = kind + '-' + op
                    prepared = images / (prefix + '-prepared.img')
                    shutil.copyfile(base, prepared)
                    if op == 'create':
                        operation(prefix + '-reserve', 'reserve', prepared, kind, reached, op)
                    operation(prefix + '-old', 'check-crash', prepared, kind, reached, op, 'old')
                    old = raw(prefix + '-old-raw', prepared)
                    complete = images / (prefix + '-complete.img')
                    shutil.copyfile(prepared, complete)
                    operation(prefix + '-mutate', 'mutate', complete, kind, reached, op)
                    operation(prefix + '-new', 'check-crash', complete, kind, reached, op, 'new')
                    new = raw(prefix + '-new-raw', complete)
                    if old['semantic_hash'] == new['semantic_hash']:
                        raise AssertionError(prefix + ': oracle must distinguish before/after')
                    complete.unlink()
                    for phase, nth, outcome in phases:
                        label = f'{prefix}-{phase}-{nth}'
                        image = images / (label + '.img')
                        shutil.copyfile(prepared, image)
                        operation(label + '-crash', 'crash', image, kind, reached, op, phase, nth, expected=86)
                        for repeat in range(2):
                            operation(label + f'-reopen-{repeat}', 'check-crash', image, kind, reached, op, outcome)
                            observed = raw(label + f'-raw-{repeat}', image)
                            if observed != (old if outcome == 'old' else new):
                                raise AssertionError(f'{label}: recovered metadata differs from {outcome} oracle: {observed}')
                        # Exercise another allocation/deallocation after replay.
                        operation(label + '-reuse', 'reuse', image, kind, reached, op)
                        raw(label + '-reuse-raw', image)
                        image.unlink()
                    prepared.unlink()
                base.unlink()
        shutil.rmtree(images)
    except Exception as error:
        if not suite.results or suite.results[-1]['status'] != 'fail':
            suite.record('stress', 'fail', error=str(error))
        print(f'Failure evidence retained: {suite.root}', file=sys.stderr)
        return 1
    finally:
        suite.save()
        print(f'Results: {suite.root}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
