#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate a small DiskANN fixture or verify its actual search and reader path."""
import argparse
import gc
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'search'))
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--threads', type=int, default=2)
    parser.add_argument('--minimum-recall', type=float, default=0.95)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output must be a new file')
    if args.rows < 1024 or args.threads < 1 or not 0 <= args.minimum_recall <= 1:
        parser.error('rows >= 1024, threads >= 1 and recall in [0, 1] are required')
    if args.action == 'search' and os.environ.get('NVAI_READER_MAP'):
        evidence = os.environ.get('NVAI_READER_EVIDENCE')
        if not evidence or Path(evidence).exists():
            parser.error('native search requires a new NVAI_READER_EVIDENCE file')
    import numpy as np
    import diskannpy

    directory = args.directory.resolve()
    if args.action == 'prepare':
        if os.environ.get('NVAI_READER_MAP') or os.environ.get('LD_PRELOAD'):
            parser.error('prepare the POSIX index without reader preloading')
        directory.mkdir(parents=True, exist_ok=False)
        rng = np.random.default_rng(20261116)
        vectors = rng.normal(size=(args.rows, 64)).astype(np.float32)
        queries = vectors[:16] + rng.normal(0, 0.01, size=(16, 64)).astype(np.float32)
        truth = np.array([np.argsort(np.sum((vectors-q)**2, axis=1))[:10]
                          for q in queries], dtype=np.uint32)
        for name, array in [('vectors', vectors), ('queries', queries), ('truth', truth)]:
            np.save(directory/(name+'.npy'), array)
        diskannpy.build_disk_index(vectors, 'l2', str(directory), 100, 32,
                                  0.1, 0.5, args.threads, index_prefix='release')
        graph = directory/'release_disk.index'
        if not graph.is_file() or not graph.stat().st_size or graph.stat().st_size % 4096:
            raise RuntimeError('expected a nonempty 4096-byte-padded DiskANN graph')
        report = {'status': 'passed', 'action': 'prepare', 'rows': args.rows,
                  'dimensions': 64, 'graph': str(graph), 'graph_bytes': graph.stat().st_size}
    else:
        queries = np.load(directory/'queries.npy')
        truth = np.load(directory/'truth.npy')
        index = diskannpy.StaticDiskIndex(str(directory), args.threads, 0,
                                         cache_mechanism=0, index_prefix='release')
        found = [index.search(q, 10, 100, beam_width=4).identifiers for q in queries]
        recall = float(np.mean([len(set(x)&set(y))/10 for x,y in zip(found,truth)]))
        del index
        gc.collect()
        report = {'status': 'passed', 'action': 'search', 'queries': len(queries),
                  'recall_at_10': recall, 'minimum_recall': args.minimum_recall}
        if recall < args.minimum_recall:
            raise RuntimeError('recall is below the required threshold: '+str(recall))
        if os.environ.get('NVAI_READER_MAP'):
            import ctypes
            library = ctypes.CDLL(None)
            shutdown = library.nvai_reader_shutdown
            shutdown.restype = ctypes.c_int
            rc = shutdown()
            if rc:
                raise RuntimeError('native reader did not shut down cleanly: '+str(rc))
            evidence = Path(os.environ['NVAI_READER_EVIDENCE'])
            records = [json.loads(line) for line in evidence.read_text().splitlines()]
            final = [row for row in records if row['event'] == 'shutdown']
            if not final or final[-1]['bytes'] <= 0 or final[-1]['reads'] <= 0:
                raise RuntimeError('no actual native graph-read/shutdown evidence')
            report['native_reader'] = final[-1]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
