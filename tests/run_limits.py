#!/usr/bin/env python3
"""Exhaust metadata-only images; verify real limits and retain failure evidence."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import sys

from run_framework import Suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build/local/limit_test')
    parser.add_argument('--checker', default='build/local/metadata_check')
    parser.add_argument('--image-mib', nargs='+', type=int, default=[128, 256])
    parser.add_argument('--cases', nargs='+', choices=['files', 'sharded', 'dirs', 'size'],
                        default=['files', 'sharded', 'dirs', 'size'])
    parser.add_argument('--report-dir', type=Path, default=Path(os.environ.get('NVFUSE_TEST_REPORT_ROOT', 'build/test-results')) /
                        ('limits-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    parser.add_argument('--keep-images', action='store_true')
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if any(n < 128 or n % 128 for n in args.image_mib) or args.timeout < 1:
        parser.error('image sizes must be positive multiples of 128 MiB; timeout must be positive')
    binary, checker = str(Path(args.binary).resolve()), str(Path(args.checker).resolve())
    suite = Suite(args.report_dir)
    images = suite.root / 'images'
    images.mkdir()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    (suite.root / 'run.json').write_text(json.dumps(dict(platform=platform.platform(),
        image_mib=args.image_mib, cases=args.cases, binary=binary, checker=checker,
        payload_mode='metadata-only; ordinary logical block accounting retained'), indent=2) + '\n')
    observed = []
    try:
        for mib in args.image_mib:
            for kind in args.cases:
                prefix = f'{mib}-{kind}'
                image = images / (prefix + '.img')
                suite.command(prefix + '-create', [binary, 'create', str(image), kind, str(mib)],
                              marker='PASS limit create', timeout=args.timeout)
                log = (suite.root / (prefix + '-create.log')).read_text(errors='replace')
                row = json.loads(next(line[6:] for line in log.splitlines() if line.startswith('LIMIT ')))
                row['host_allocated_bytes'] = image.stat().st_blocks * 512
                row['logical_image_bytes'] = image.stat().st_size
                if kind == 'size':
                    if row['payload_discarded'] < row['reached'] or row['host_allocated_bytes'] >= row['reached']:
                        raise AssertionError('metadata-only payload/storage contract failed')
                elif row['payload_discarded']:
                    raise AssertionError('empty-file/directory limit test unexpectedly wrote payload')
                observed.append(row)
                (suite.root / 'limits.json').write_text(json.dumps(observed, indent=2) + '\n')
                suite.record(prefix + '-measured-limit', 'pass', **row)
                suite.command(prefix + '-raw-check', [checker, str(image)], marker='CHECK ', timeout=args.timeout)
                # Confirm the checker detects an independent bitmap mutation.
                # Restore the disposable image even if the negative check fails.
                if kind == args.cases[0] and mib == args.image_mib[0]:
                    with image.open('r+b') as damaged:
                        damaged.seek(3 * 4096)
                        original = damaged.read(1)
                        try:
                            damaged.seek(3 * 4096)
                            damaged.write(bytes([original[0] ^ 1]))
                            damaged.flush()
                            os.fsync(damaged.fileno())
                            suite.command(prefix + '-checker-rejects-corruption', [checker, str(image)],
                                          expected=1, marker='CHECK FAILED', timeout=args.timeout)
                        finally:
                            damaged.seek(3 * 4096)
                            damaged.write(original)
                            damaged.flush()
                            os.fsync(damaged.fileno())
                for repeat in range(2):
                    suite.command(prefix + f'-remount-{repeat}',
                                  [binary, 'verify', str(image), kind, str(mib), str(row['reached'])],
                                  marker='PASS limit verify', timeout=args.timeout)
                    suite.command(prefix + f'-raw-check-{repeat}', [checker, str(image)],
                                  marker='CHECK ', timeout=args.timeout)
                for cycle in range(2):
                    label = prefix + f'-cleanup-{cycle}'
                    suite.command(label, [binary, 'cleanup', str(image), kind, str(mib), str(row['reached'])],
                                  marker='PASS limit cleanup', timeout=args.timeout)
                    cleanup_log = (suite.root / (label + '.log')).read_text(errors='replace')
                    empty = json.loads(next(line[8:] for line in cleanup_log.splitlines() if line.startswith('CLEANUP ')))
                    if (empty['free_blocks'], empty['free_inodes']) != (row['baseline_blocks'], row['baseline_inodes']):
                        raise AssertionError(f'{label}: resources not fully reclaimed: {empty}, baseline={row}')
                    suite.command(label + '-raw-check', [checker, str(image)], marker='CHECK ', timeout=args.timeout)
                    if cycle == 0:
                        suite.command(prefix + '-refill', [binary, 'refill', str(image), kind, str(mib)],
                                      marker='PASS limit refill', timeout=args.timeout)
                        refill_log = (suite.root / (prefix + '-refill.log')).read_text(errors='replace')
                        refill = json.loads(next(line[6:] for line in refill_log.splitlines() if line.startswith('LIMIT ')))
                        if refill['reached'] != row['reached']:
                            raise AssertionError(f'{prefix}: refill capacity changed: {refill}')
                        suite.command(prefix + '-refill-raw-check', [checker, str(image)], marker='CHECK ', timeout=args.timeout)
        if not args.keep_images:
            shutil.rmtree(images)
    except Exception as error:
        if not suite.results or suite.results[-1]['status'] != 'fail':
            suite.record('limits', 'fail', error=str(error))
        print(f'Failure evidence retained: {suite.root}', file=sys.stderr)
        return 1
    finally:
        suite.save()
        print(f'Results: {suite.root}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
