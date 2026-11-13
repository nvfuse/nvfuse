#!/usr/bin/env python3
"""Summarize sampled real-function timers (nested elapsed times overlap)."""
import argparse
import json
from pathlib import Path


def summarize(log):
    groups = {}
    for line in log.read_text(errors='replace').splitlines():
        if not line.startswith('NVFUSE_DETAIL '):
            continue
        record = dict(item.split('=', 1) for item in line.split()[1:])
        count = int(record.pop('count'))
        opcode = record.pop('opcode')
        row = groups.setdefault(opcode, {'samples': 0, 'functions': {}})
        row['samples'] += count
        for key, value in record.items():
            name, unit = key.rsplit('_', 1)
            if unit not in ('ns', 'calls'):
                raise ValueError('unknown detail field: ' + key)
            function = row['functions'].setdefault(name, {})
            function[unit] = function.get(unit, 0.0) + count * float(value)
    for row in groups.values():
        for function in row['functions'].values():
            if row['samples']:
                for unit in function:
                    function[unit] /= row['samples']
    return groups


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    rows = []
    for log in sorted(args.directory.glob('*.log')):
        groups = summarize(log)
        if groups:
            rows.append({'name': log.stem, 'details': groups})
    print(json.dumps({'note': 'Nested elapsed ns overlap; do not add functions. '
                      'Calls and ns are accumulated per sampled request, '
                      'including diagnostic overhead.', 'workloads': rows}, indent=2))


if __name__ == '__main__':
    main()
