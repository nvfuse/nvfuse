#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create a small DISKANN collection or verify it through the real Milvus RPC API."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'verify'))
    parser.add_argument('--uri', required=True)
    parser.add_argument('--collection', required=True)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--minimum-recall', type=float, default=0.95)
    parser.add_argument('--timeout', type=float, help='RPC seconds: prepare 600, verify 60 by default')
    parser.add_argument('--reader-evidence', type=Path)
    parser.add_argument('--keep-loaded', action='store_true',
                        help='prepare only: keep the collection loaded for graph discovery')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output must be a new file')
    if args.rows < 1024 or not 0 <= args.minimum_recall <= 1:
        parser.error('rows >= 1024 and recall in [0, 1] are required')
    if args.keep_loaded and (args.action != 'prepare' or args.reader_evidence):
        parser.error('keep-loaded is only for POSIX fixture preparation')
    if args.timeout is not None and args.timeout <= 0:
        parser.error('timeout must be positive')
    timeout = args.timeout if args.timeout is not None else (600 if args.action == 'prepare' else 60)
    import numpy as np
    from pymilvus import DataType, MilvusClient

    directory = args.directory.resolve()
    client = MilvusClient(uri=args.uri, timeout=timeout)
    loaded = False
    try:
        if args.action == 'prepare':
            if client.has_collection(args.collection):
                raise RuntimeError('choose a new private collection name')
            directory.mkdir(parents=True, exist_ok=False)
            rng = np.random.default_rng(20261116)
            vectors = rng.normal(size=(args.rows, 64)).astype(np.float32)
            queries = vectors[:16] + rng.normal(0, 0.01, size=(16, 64)).astype(np.float32)
            truth = np.array([np.argsort(np.sum((vectors-q)**2, axis=1))[:10]
                              for q in queries], dtype=np.int64)
            np.save(directory/'queries.npy', queries)
            np.save(directory/'truth.npy', truth)
            schema = client.create_schema(auto_id=False, enable_dynamic_field=False)
            schema.add_field('id', DataType.INT64, is_primary=True)
            schema.add_field('vector', DataType.FLOAT_VECTOR, dim=64)
            client.create_collection(args.collection, schema=schema, consistency_level='Strong')
            for start in range(0, len(vectors), 1000):
                client.insert(args.collection, [{'id': i, 'vector': vectors[i].tolist()}
                                               for i in range(start, min(start+1000,len(vectors)))])
            client.flush(args.collection)
            index = client.prepare_index_params()
            index.add_index(field_name='vector', index_type='DISKANN', metric_type='L2')
            client.create_index(args.collection, index)
        else:
            if not client.has_collection(args.collection):
                raise RuntimeError('prepared collection is missing')
            queries = np.load(directory/'queries.npy')
            truth = np.load(directory/'truth.npy')
        print('Loading collection', flush=True)
        client.load_collection(args.collection, timeout=timeout)
        loaded = True
        print('Verifying RPC search', flush=True)
        results = client.search(args.collection, queries.tolist(), anns_field='vector',
                                limit=10, search_params={'metric_type': 'L2',
                                                        'params': {'search_list': 100}}, timeout=timeout)
        if len(results) != len(queries):
            raise RuntimeError('unexpected result count')
        recall = float(np.mean([len({int(hit['id']) for hit in hits}&set(expected))/10
                                for hits,expected in zip(results,truth)]))
        if recall < args.minimum_recall:
            raise RuntimeError('recall is below the required threshold: '+str(recall))
        report = {'status': 'passed', 'action': args.action, 'collection': args.collection,
                  'queries': len(queries), 'index_type': 'DISKANN', 'recall_at_10': recall,
                  'minimum_recall': args.minimum_recall, 'transport': 'Milvus RPC'}
    finally:
        try:
            if loaded and not args.keep_loaded:
                print('Releasing collection', flush=True)
                client.release_collection(args.collection, timeout=min(timeout, 30))
        finally:
            client.close()
    if args.reader_evidence:
        records = [json.loads(line) for line in args.reader_evidence.read_text().splitlines()]
        closed = [row for row in records if row['event'] == 'close']
        if not closed or closed[-1]['bytes'] <= 0 or closed[-1]['reads'] <= 0:
            raise RuntimeError('no actual native graph-read/close evidence')
        report['native_reader'] = closed[-1]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
