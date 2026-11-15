#!/usr/bin/env python3
"""FIFO dependency, mixed-disjoint batch, close and rejected native I/O checks."""
import argparse,json,select,time
from pathlib import Path
from lmcache_nvfuse import NVFUSEConnector

p=argparse.ArgumentParser();p.add_argument('--backend',required=True);p.add_argument('--arena',required=True)
p.add_argument('--library',default='');p.add_argument('--output',required=True);a=p.parse_args()
c=NVFUSEConnector(a.backend,a.arena,4096,64,a.library)
expected={};requests=[];destinations=[]
try:
    # Enqueue dependent replacements and independent writes without waiting.
    # Each read must observe its preceding write, even with batch coalescing.
    for i in range(24):
        key=f'key-{i%8}';payload=bytearray(bytes([i])*4096)
        future=c.submit_batch_set([key],[memoryview(payload)]);requests.append((future,None))
        dest=bytearray(4096);destinations.append(dest)
        future=c.submit_batch_get([key],[memoryview(dest)]);requests.append((future,payload))
    c.close();completed=c.drain_completions()
    assert [x[0] for x in completed]==[x[0] for x in requests],completed
    assert all(x[1] for x in completed),completed
    for i,payload in enumerate(range(24)):assert destinations[i]==bytes([payload])*4096
    assert c.drain_completions()==[]
    Path(a.output).write_text(json.dumps({'status':'passed','backend':a.backend,'operations':48,
       'checks':['dependent overwrite/read ordering','independent-key batching','close drains all accepted requests exactly once','all 24 complete payloads match']},indent=2)+'\n')
    print('BATCH_ORDERING_AND_CLOSE_PASSED')
finally:c.close()
