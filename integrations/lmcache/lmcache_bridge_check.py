#!/usr/bin/env python3
"""Use the real pinned LMCache factory, ObjectKey, CPU TensorMemoryObj and L2 bridge.
No mocks or replacement LMCache modules are injected by this check.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import time
import torch
from lmcache.v1.distributed.api import ObjectKey
from lmcache.v1.distributed.l2_adapters.native_plugin_l2_adapter import (
    NativePluginL2AdapterConfig, _create_native_plugin_l2_adapter)
from lmcache.v1.memory_management import TensorMemoryObj, MemoryObjMetadata, MemoryFormat


def tensor_obj(tensor):
    metadata=MemoryObjMetadata(shape=tensor.shape,dtype=tensor.dtype,address=0,
        phy_size=tensor.numel()*tensor.element_size(),ref_count=1,pin_count=0,fmt=MemoryFormat.BINARY_BUFFER)
    return TensorMemoryObj(tensor,metadata,None)


def wait(fd, query):
    deadline=time.monotonic()+60
    poller=select.poll()
    poller.register(fd,select.POLLIN)
    while time.monotonic()<deadline:
        result=query()
        if result is not None:
            return result
        if poller.poll(500):
            os.read(fd,8)
    raise TimeoutError('LMCache L2 bridge did not finish')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--backend',required=True)
    p.add_argument('--arena',required=True)
    p.add_argument('--library',default='')
    p.add_argument('--output',required=True)
    a=p.parse_args()
    config=NativePluginL2AdapterConfig(module_path='lmcache_nvfuse',class_name='NVFUSEConnector',
        adapter_params={'backend':a.backend,'arena_path':a.arena,'object_bytes':262144,
            'slots':64,'library':a.library})
    adapter=_create_native_plugin_l2_adapter(config)
    try:
        keys=[ObjectKey(chunk_hash=hashlib.sha256(f'kv-{i}'.encode()).digest(),
            model_name='cpu-kv-fixture-20261114/uint8-layout',kv_rank=0,cache_salt='isolated-experiment') for i in range(3)]
        source=[tensor_obj(torch.arange(262144,dtype=torch.int32).to(torch.uint8)+i) for i in range(3)]
        destination=[tensor_obj(torch.zeros(262144,dtype=torch.uint8)) for _ in keys]
        store=adapter.submit_store_task(keys,source)
        result=wait(adapter.get_store_event_fd(),lambda:adapter.pop_completed_store_tasks().get(store))
        assert result.is_successful() and result.bytes_transferred()==3*262144
        lookup=adapter.submit_lookup_and_lock_task(keys,{})
        bitmap=wait(adapter.get_lookup_and_lock_event_fd(),lambda:adapter.query_lookup_and_lock_result(lookup))
        assert bitmap.popcount()==3
        load=adapter.submit_load_task(keys,destination)
        bitmap=wait(adapter.get_load_event_fd(),lambda:adapter.query_load_result(load))
        assert bitmap.popcount()==3
        assert all(torch.equal(x.raw_data,y.raw_data) for x,y in zip(source,destination))
        adapter.submit_unlock(keys)
        adapter.delete(keys)
        lookup=adapter.submit_lookup_and_lock_task(keys,{})
        bitmap=wait(adapter.get_lookup_and_lock_event_fd(),lambda:adapter.query_lookup_and_lock_result(lookup))
        assert bitmap.popcount()==0
        report={'integration':'actual LMCache native_plugin factory and NativeConnectorL2Adapter',
            'lmcache_commit':'bcafb031401d404a5dabae4853f5e23bf9e5685c','backend':a.backend,
            'torch':torch.__version__,'device':'cpu','keys':len(keys),'payload_bytes':3*262144,
            'checks':['async store and byte accounting','ObjectKey serialization','lookup and lock',
                'CPU TensorMemoryObj load equality','unlock/delete','miss after delete'],
            'status':'passed'}
        Path(a.output).write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report),flush=True)
    finally:
        adapter.close()
