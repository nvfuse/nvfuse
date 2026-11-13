#!/usr/bin/env python3
"""Create an isolated primary build with sampled 4K latency instrumentation.

Never edits source_dir. Rebuild the archive and engine together in output_dir.
The diagnostic layout is intentionally not a public ABI or production feature.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def instrument(source_dir, output_dir):
    source_dir, output_dir = source_dir.resolve(), output_dir.resolve()
    if output_dir.exists():
        raise ValueError('output directory must be new')
    output_dir.mkdir(parents=True)
    files = list(source_dir.glob('*.c')) + [source_dir / 'Makefile.spdk']
    files += list((source_dir / 'include').glob('*.h'))
    files += [p for p in (source_dir / 'examples/fio_plugin/modern').iterdir()
              if p.suffix in ('.c', '.h', '.map') or p.name == 'Makefile']
    manifest = {}
    for src in files:
        rel = src.relative_to(source_dir)
        dst = output_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        manifest[str(rel)] = hashlib.sha256(src.read_bytes()).hexdigest()

    def patch(name, before, after):
        p = output_dir / name
        text = p.read_text()
        if text.count(before) != 1:
            raise ValueError(f'{name}: expected unique instrumentation anchor: {before!r}')
        p.write_text(text.replace(before, after))

    patch('include/nvfuse_aio.h', '\tu64 submit_tsc;', '\tu64 *stage_tsc; /* diagnostic copy only */\n\tu64 submit_tsc;')
    patch('include/nvfuse_io_manager.h', 'struct io_job {', 'struct io_job {\n    uint64_t *stage_tsc; /* diagnostic copy only */')
    patch('nvfuse_core.c', '\tres = rte_mempool_get_bulk((struct rte_mempool *)sb->io_job_mempool, (void **)jobs, numjobs);',
          '\tres = rte_mempool_get_bulk((struct rte_mempool *)sb->io_job_mempool, (void **)jobs, numjobs);\n    if (!res) for (int i = 0; i < numjobs; i++) jobs[i]->stage_tsc = NULL;')
    patch('nvfuse_reactor.c', '\treq->iovcnt = 1;', '\treq->stage_tsc = NULL;\n\treq->iovcnt = 1;')
    patch('nvfuse_reactor.c', '\ttarget = task->target;\n', '\ttarget = task->target;\n    if (req->stage_tsc) req->stage_tsc[6] = spdk_get_ticks();\n')
    patch('nvfuse_reactor.c', '\t\ttarget->current_queue_depth++;', '\t\tif (req->stage_tsc) req->stage_tsc[5] = spdk_get_ticks();\n\t\ttarget->current_queue_depth++;')
    patch('nvfuse_aio.c', '\t\tjobs[job_count]->tag1 = (void *)areq;', '\t\tjobs[job_count]->tag1 = (void *)areq;\n        jobs[job_count]->stage_tsc = areq->stage_tsc;')
    inode_anchor = ('    if (!ictx && !fast_read) goto free_jobs;'
                    if 'if (!ictx && !fast_read)' in (output_dir / 'nvfuse_aio.c').read_text()
                    else '    if (!ictx) goto free_jobs;')
    patch('nvfuse_aio.c', inode_anchor, inode_anchor + '\n    if (areq->stage_tsc) areq->stage_tsc[2] = spdk_get_ticks();')
    # Mapping ends before overwrite timestamp/size accounting. Older sources
    # combined the write and size predicates; both use the same stage boundary.
    mapping_end = ('#ifndef NVFUSE_FILE_BACKEND\n    if (write) {'
                   if '#ifndef NVFUSE_FILE_BACKEND\n    if (write) {' in
                   (output_dir / 'nvfuse_aio.c').read_text() else
                   '    if (write && ictx->ictx_inode->i_size < areq->offset + areq->bytes)')
    patch('nvfuse_aio.c', mapping_end,
          '    if (areq->stage_tsc) areq->stage_tsc[3] = spdk_get_ticks();\n' + mapping_end)
    patch('nvfuse_aio.c', '\tres = reactor_submit_reqs(sb->target, aioq->task, jobs, job_count);',
          '\tif (areq->stage_tsc) areq->stage_tsc[4] = spdk_get_ticks();\n\tres = reactor_submit_reqs(sb->target, aioq->task, jobs, job_count);')
    runtime = 'examples/fio_plugin/modern/runtime.c'
    patch(runtime, '    struct nvfio_client *next;',
          '    uint64_t profile_sequence, profile_start, profile_count[2], profile_invalid[2];\n    uint64_t profile_sum[2][8], profile_max[2][8];\n    struct nvfio_client *next;')
    patch(runtime, 'struct nvfio_request { struct nvfuse_aio_req aio;',
          'struct nvfio_request { uint64_t stage_tsc[9]; struct nvfuse_aio_req aio;')
    patch(runtime, 'static int poll_queues = 1;',
          'static int poll_queues = 1;\nstatic int profile_on;')
    patch(runtime, '    rt.started = 1; rt.format = format;',
          '    profile_on = getenv("NVFUSE_SPDK_PROFILE") && !strcmp(getenv("NVFUSE_SPDK_PROFILE"), "1");\n    rt.started = 1; rt.format = format;')
    patch(runtime, '    strcpy(rt.params.appname, "fio");', '''    if (profile_on) {
        uint64_t minimum = UINT64_MAX;
        for (unsigned i = 0; i < 10000; i++) {
            uint64_t before = spdk_get_ticks(), after = spdk_get_ticks();
            if (after - before < minimum) minimum = after - before;
        }
        printf("NVFUSE_PROFILE enabled=1 sample_interval=1024 warmup_seconds=2 bytes=4096 ticks_hz=%" PRIu64 " clock_pair_min_ns=%.3f\\n",
               spdk_get_ticks_hz(), (double)minimum * 1e9 / spdk_get_ticks_hz());
    }
    strcpy(rt.params.appname, "fio");''')
    patch(runtime, '    r->lane = lane;', '    r->lane = lane;\n    if (r->aio.stage_tsc) r->stage_tsc[1] = spdk_get_ticks();')
    patch(runtime, '    if (nvfio_ring_push(&client->pending, r)) return -EAGAIN;',
          '    if (profile_on && size == CLUSTER_SIZE) {\n'
          '        if (!client->profile_start) client->profile_start = spdk_get_ticks();\n'
          '        uint64_t sample = ++client->profile_sequence;\n'
          '        sample = (sample ^ (sample >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);\n'
          '        sample = (sample ^ (sample >> 27)) * UINT64_C(0x94d049bb133111eb);\n'
          '        sample ^= sample >> 31;\n'
          '        if (!(sample & 1023)) {\n'
          '            uint64_t now = spdk_get_ticks();\n'
          '            if (now - client->profile_start >= 2 * spdk_get_ticks_hz()) {\n'
          '                memset(r->stage_tsc, 0, sizeof(r->stage_tsc));\n'
          '                r->aio.stage_tsc = r->stage_tsc; r->stage_tsc[0] = now;\n'
          '            }\n'
          '        }\n'
          '    }\n'
          '    if (nvfio_ring_push(&client->pending, r)) return -EAGAIN;')
    patch(runtime, '            c->completed[count++] = r;',
          '            if (r->aio.stage_tsc) r->stage_tsc[7] = spdk_get_ticks();\n            c->completed[count++] = r;')
    patch(runtime, '        completed[n++] = r;', '''        if (r->aio.stage_tsc && !r->error && !r->aio.error) {
            r->stage_tsc[8] = spdk_get_ticks();
            unsigned op = r->aio.opcode == WRITE, valid = 1;
            for (unsigned i = 0; i < 8; i++)
                if (!r->stage_tsc[i] || r->stage_tsc[i+1] < r->stage_tsc[i]) valid = 0;
            if (valid) {
                client->profile_count[op]++;
                for (unsigned i = 0; i < 8; i++) {
                    uint64_t elapsed = r->stage_tsc[i+1] - r->stage_tsc[i];
                    client->profile_sum[op][i] += elapsed;
                    if (elapsed > client->profile_max[op][i]) client->profile_max[op][i] = elapsed;
                }
            } else client->profile_invalid[op]++;
        }
        completed[n++] = r;''')
    patch(runtime, '    *link = c->client->next;', '''    *link = c->client->next;
    if (profile_on) {
        static const char *stages[] = {"pending", "inode_and_jobs", "mapping", "release_and_flush", "sq_wait", "bdev", "cq_wait", "worker_wait"};
        for (unsigned op = 0; op < 2; op++) {
            uint64_t count = c->client->profile_count[op];
            printf("NVFUSE_STAGE opcode=%s count=%" PRIu64 " invalid=%" PRIu64,
                   op ? "write" : "read", count, c->client->profile_invalid[op]);
            for (unsigned i = 0; i < 8; i++)
                printf(" %s_us=%.6f %s_max_us=%.6f", stages[i],
                    count ? (double)c->client->profile_sum[op][i] * 1e6 / spdk_get_ticks_hz() / count : 0,
                    stages[i], (double)c->client->profile_max[op][i] * 1e6 / spdk_get_ticks_hz());
            puts("");
        }
    }''')
    (output_dir / 'profile-source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    instrument(args.source, args.output)
