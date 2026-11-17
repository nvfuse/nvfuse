#!/usr/bin/env python3
"""Import immutable graph files through native NVFUSE; no mount or FUSE.

--format is reserved for separately identity-checked disposable provisioning.
Ordinary imports and all readers reopen an existing volume.
"""
import argparse,ctypes as C,hashlib,json,os
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--format',action='store_true')
p.add_argument('--file',action='append',required=True);p.add_argument('--output',required=True);a=p.parse_args()
lib=C.CDLL(a.library)
lib.nvai_open_arena.argtypes=[C.c_char_p,C.c_uint64]
lib.nvai_transfer.argtypes=[C.c_int,C.c_void_p,C.c_size_t,C.c_uint64,C.c_int]
def check(rc):
    if rc<0:raise OSError(-rc,os.strerror(-rc))
check(lib.nvai_start(int(a.format)));records=[]
try:
    for spec in a.file:
        source,destination=spec.rsplit('=',1);path=Path(source);size=path.stat().st_size
        assert size and size%4096==0,'graph files must be 4KiB padded already'
        fd=lib.nvai_open_arena(destination.encode(),size);check(fd)
        digest=hashlib.sha256()
        try:
            with path.open('rb') as input:
                for offset in range(0,size,4*1024*1024):
                    data=bytearray(input.read(min(4*1024*1024,size-offset)))
                    array=(C.c_ubyte*len(data)).from_buffer(data)
                    check(lib.nvai_transfer(fd,array,len(data),offset,1));digest.update(data)
            check(lib.nvai_sync(fd))
            with path.open('rb') as input:
                for offset in range(0,size,4*1024*1024):
                    expected=input.read(min(4*1024*1024,size-offset));data=bytearray(len(expected))
                    array=(C.c_ubyte*len(data)).from_buffer(data)
                    check(lib.nvai_transfer(fd,array,len(data),offset,0));assert data==expected,(source,offset)
        finally:check(lib.nvai_close(fd))
        records.append({'source':source,'nvfuse_path':destination,'bytes':size,'sha256':digest.hexdigest(),'readback':'all bytes equal'})
        print('NATIVE_GRAPH_IMPORT_VERIFIED',destination,size,flush=True)
finally:check(lib.nvai_stop())
Path(a.output).write_text(json.dumps({'status':'passed','format':a.format,'files':records},indent=2)+'\n')
