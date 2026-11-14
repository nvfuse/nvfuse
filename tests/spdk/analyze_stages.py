#!/usr/bin/env python3
"""Summarize logs from the isolated SPDK stage instrumentation build."""
import argparse
import json
from pathlib import Path

STAGES = ('pending', 'inode_and_jobs', 'mapping', 'release_and_flush',
          'sq_wait', 'bdev', 'cq_wait', 'worker_wait')


def summarize(log):
    groups = {}
    for line in log.read_text(errors='replace').splitlines():
        if not line.startswith('NVFUSE_STAGE '):
            continue
        record = dict(item.split('=', 1) for item in line.split()[1:])
        opcode, count = record['opcode'], int(record['count'])
        row = groups.setdefault(opcode, {'count': 0, 'invalid': 0,
                                         'mean_us': dict.fromkeys(STAGES, 0.0),
                                         'max_us': dict.fromkeys(STAGES, 0.0)})
        row['count'] += count
        row['invalid'] += int(record['invalid'])
        for stage in STAGES:
            row['mean_us'][stage] += count * float(record[stage + '_us'])
            row['max_us'][stage] = max(row['max_us'][stage], float(record[stage + '_max_us']))
    for row in groups.values():
        if row['count']:
            row['mean_us'] = {k: v / row['count'] for k, v in row['mean_us'].items()}
            row['total_us'] = sum(row['mean_us'].values())
            row['fs_prepare_us'] = sum(row['mean_us'][k] for k in STAGES[1:4])
    return groups


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='collected physical evidence directory')
    args = parser.parse_args()
    output = []
    for log in sorted(args.directory.glob('*.log')):
        groups = summarize(log)
        if not groups:
            continue
        row = {'name': log.stem, 'stages': groups}
        path = log.with_suffix('.json')
        if path.exists():
            raw = path.read_text()
            job = json.loads(raw[raw.index('{'):])['jobs'][0]
            row['fio_error'] = job['error']
            row['iops'] = sum(job[op]['iops'] for op in ('read', 'write'))
            row['fio_mean_clat_us'] = {op: job[op]['clat_ns']['mean'] / 1000 for op in ('read', 'write')}
            # Approximate demand, not measured utilization: sampled elapsed
            # preparation includes stalls; runtime intervals may differ slightly.
            row['estimated_fs_prepare_core_seconds_per_second'] = sum(
                job[op]['iops'] * groups.get(op, {}).get('fs_prepare_us', 0) / 1e6
                for op in ('read', 'write'))
        output.append(row)
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
