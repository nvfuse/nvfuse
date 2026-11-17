#!/usr/bin/env python3
"""Capacity, reclamation, close/drain and cold-namespace integration checks."""
import argparse
import json
from pathlib import Path
import select
import time
from lmcache_nvfuse import NVFUSEConnector


def completion(c,future,success=True):
    poller=select.poll();poller.register(c.event_fd(),select.POLLIN)
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        if poller.poll(500):
            result=c.drain_completions()
            assert len(result)==1 and result[0][0]==future
            assert result[0][1] is success,result
            return result[0][3]
    raise TimeoutError('accepted work had no terminal completion')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--backend',required=True)
    p.add_argument('--arena',required=True)
    p.add_argument('--library',default='')
    p.add_argument('--output',required=True)
    p.add_argument('--cold-only',action='store_true')
    a=p.parse_args()
    c=NVFUSEConnector(a.backend,a.arena,4096,4,a.library)
    checks=[]
    try:
        assert completion(c,c.submit_batch_exists(['a','b','c','d']))==[False]*4
        checks.append('fresh process namespace reports retained slots as misses')
        if not a.cold_only:
            source=[bytearray(bytes([i])*4096) for i in range(4)]
            completion(c,c.submit_batch_set(['a','b','c','d'],list(map(memoryview,source))))
            completion(c,c.submit_batch_set(['overflow'],[memoryview(source[0])]),False)
            assert completion(c,c.submit_batch_delete(['a']))==[True]
            completion(c,c.submit_batch_set(['replacement'],[memoryview(source[0])]))
            dest=[bytearray(4096) for _ in range(4)]
            assert completion(c,c.submit_batch_get(['replacement','b','c','d'],list(map(memoryview,dest))))==[True]*4
            assert source==dest
            checks.extend(['logical capacity fails safely','delete reclaims slot','reused slot has correct complete bytes'])
            futures=[]
            for _ in range(8):
                futures.append(c.submit_batch_get(['b'],[memoryview(bytearray(4096))]))
            c.close()
            results=c.drain_completions()
            assert sorted(x[0] for x in results)==futures
            assert all(x[1] and x[3]==[True] for x in results)
            assert c.drain_completions()==[]
            try:c.submit_batch_exists(['b'])
            except RuntimeError:pass
            else:raise AssertionError('submission accepted after close')
            checks.extend(['close retires all accepted work exactly once','drain is idempotent','closed connector rejects new work'])
        report={'backend':a.backend,'status':'passed','checks':checks}
        Path(a.output).write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report),flush=True)
    finally:c.close()
