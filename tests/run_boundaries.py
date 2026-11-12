#!/usr/bin/env python3
"""Retain full-geometry and journal-capacity boundary evidence."""
import argparse
import datetime
import os
from pathlib import Path
import sys
from run_framework import Suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build/local')
    parser.add_argument('--report-dir', type=Path, default=Path(os.environ.get('NVFUSE_TEST_REPORT_ROOT', 'build/test-results')) /
                        ('boundaries-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    args = parser.parse_args()
    suite = Suite(args.report_dir)
    build = Path(args.build_dir).resolve()
    try:
        suite.command('indirect-full-two-cycles', [str(build / 'indirect_boundary_test'), '--full', '2'],
                      marker='PASS full cycle=1', timeout=180)
        suite.command('journal-capacity', [str(build / 'journal_boundary_test')],
                      marker='PASS journal final-slot crash=3', timeout=180)
    except Exception as error:
        if not suite.results or suite.results[-1]['status'] != 'fail':
            suite.record('boundaries', 'fail', error=str(error))
        print(f'Failure evidence retained: {suite.root}', file=sys.stderr)
        return 1
    finally:
        suite.save()
        print(f'Results: {suite.root}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
