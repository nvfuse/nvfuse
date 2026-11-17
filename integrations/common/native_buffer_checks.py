#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check native DMA sizing, reuse, growth and failure drain on an existing graph."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes as c
import errno
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--graph', required=True, help='existing immutable NVFUSE path')
    p.add_argument('--expected', type=Path, required=True, help='original graph bytes')
    p.add_argument('--contexts', type=int, default=12)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.output.exists() or not 2 <= a.contexts <= 32:
        p.error('choose a new output and 2..32 contexts')
    expected = a.expected.read_bytes()
    if len(expected) < 512 * 1024 or len(expected) % 4096:
        p.error('expected graph must be at least 512 KiB and 4096-byte aligned')
    probe = c.CDLL(None)
    probe.nvai_probe_reset.argtypes = [c.c_long]
    probe.nvai_probe_calls.restype = probe.nvai_probe_bytes.restype = c.c_long
    lib = c.CDLL(str(a.library.resolve()), mode=c.RTLD_GLOBAL)
    lib.nvai_start.argtypes = [c.c_int]
    lib.nvai_open_read.argtypes = [c.c_char_p]
    lib.nvai_context_create.restype = c.c_void_p
    lib.nvai_context_free.argtypes = [c.c_void_p]
    lib.nvai_close.argtypes = [c.c_int]
    lib.nvai_transfer_batch_context.argtypes = [
        c.c_void_p, c.c_int, c.POINTER(c.c_void_p), c.POINTER(c.c_size_t),
        c.POINTER(c.c_uint64), c.POINTER(c.c_int), c.c_uint,
    ]

    def check(rc):
        if rc < 0:
            raise RuntimeError('native call failed: '+str(rc))
        return rc

    def read(context, size=4096, count=32, failure=False):
        buffers = [c.create_string_buffer(size) for _ in range(count)]
        offsets = [(i*4096) % (len(expected)-size+4096) for i in range(count)]
        rc = lib.nvai_transfer_batch_context(
            context, fd, (c.c_void_p*count)(*[c.addressof(b) for b in buffers]),
            (c.c_size_t*count)(*[size]*count), (c.c_uint64*count)(*offsets),
            (c.c_int*count)(*[0]*count), count)
        if failure:
            if rc != -errno.ENOMEM:
                raise RuntimeError('expected injected ENOMEM, got '+str(rc))
        else:
            check(rc)
            for b, offset in zip(buffers, offsets):
                if b.raw != expected[offset:offset+size]:
                    raise RuntimeError('graph payload mismatch')

    contexts = []
    fd = -1
    check(lib.nvai_start(0))
    try:
        fd = check(lib.nvai_open_read(a.graph.encode()))
        probe.nvai_probe_reset(-1)
        for _ in range(a.contexts):
            context = lib.nvai_context_create()
            if not context:
                raise RuntimeError('context allocation failed')
            contexts.append(context)
        if probe.nvai_probe_calls() != 0:
            raise RuntimeError('idle contexts allocated DMA')
        with ThreadPoolExecutor(max_workers=a.contexts) as workers:
            list(workers.map(read, contexts))
        small_bytes = probe.nvai_probe_bytes()
        if small_bytes != a.contexts*32*4096:
            raise RuntimeError('small reads reserved oversized DMA buffers')
        probe.nvai_probe_reset(-1)
        read(contexts[0], 256*1024, 1)
        read(contexts[0], 4096, 1)
        if probe.nvai_probe_calls() != 1 or probe.nvai_probe_bytes() != 256*1024:
            raise RuntimeError('idle-slot growth/reuse failed')
        context = lib.nvai_context_create()
        if not context:
            raise RuntimeError('failure-test context allocation failed')
        contexts.append(context)
        probe.nvai_probe_reset(2)
        read(context, count=4, failure=True)
        # Free must succeed after two accepted reads and a failed third DMA
        # allocation: returning ENOMEM alone does not establish DMA lifetime.
        check(lib.nvai_context_free(context))
        contexts.pop()
        probe.nvai_probe_reset(-1)
        read(contexts[0], 256*1024, 32)
    finally:
        probe.nvai_probe_reset(-1)
        for context in contexts:
            check(lib.nvai_context_free(context))
        if fd >= 0:
            check(lib.nvai_close(fd))
        check(lib.nvai_stop())
    report = {'status': 'passed', 'contexts': a.contexts,
              'small_batch_dma_bytes': small_bytes, 'small_read_bytes': 4096,
              'checks': ['idle allocation', 'parallel payloads', 'slot growth/reuse',
                         'allocation failure drains accepted I/O', '32x256KiB batch',
                         'context free and runtime shutdown']}
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
