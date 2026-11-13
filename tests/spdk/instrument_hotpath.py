#!/usr/bin/env python3
"""Add sampled real-function timers to an isolated primary stage-profile copy."""
import argparse
from pathlib import Path
import re
from instrument_primary import instrument


def instrument_hotpath(source, output):
    instrument(source, output)
    names = ['inode_read','ictx_get','bh_find','bh_alloc','bc_get','bc_hash','bc_move','bh_release','map_total','bh_total']
    header='''#ifndef NVFUSE_HOT_PROFILE_H
#define NVFUSE_HOT_PROFILE_H
#include <stdint.h>
extern _Thread_local uint64_t *nvfuse_hot_profile;
#endif
'''
    (output/'include/nvfuse_hot_profile.h').write_text(header)
    descriptors=[
        ('nvfuse_core.c','nvfuse_read_inode','sb, ictx_given, ino',False),
        ('nvfuse_inode_cache.c','nvfuse_get_ictx','sb, ino',False),
        ('nvfuse_buffer_cache.c','nvfuse_find_bh_in_ictx','sb, ictx, ino, lbno',False),
        ('nvfuse_buffer_cache.c','nvfuse_alloc_buffer_head','sb',False),
        ('nvfuse_buffer_cache.c','nvfuse_get_bc','sb, ictx, ino, lblock, sync_read',False),
        ('nvfuse_buffer_cache.c','nvfuse_hash_lookup','bm, key',False),
        ('nvfuse_buffer_cache.c','nvfuse_move_buffer_list_nolock','sb, bc, desired_type, tail',True),
        ('nvfuse_buffer_cache.c','nvfuse_release_bh','sb, bh, tail, dirty',True),
        ('nvfuse_indirect.c','nvfuse_get_block','sb, ictx, lblock, maxblocks, num_alloc_blocks, pblock, create',False),
        ('nvfuse_buffer_cache.c','_nvfuse_get_bh','sb, ictx, ino, lblock, sync_read, is_meta',False)]
    if 'nvfuse_get_block_cached(' in (output/'nvfuse_indirect.c').read_text():
        descriptors[8]=('nvfuse_indirect.c','nvfuse_get_block_cached','sb, ictx, lblock, maxblocks, count, physical',False)
    if 'nvfuse_try_cached_read_mapping(' in (output/'nvfuse_indirect.c').read_text():
        # This includes inode acquisition, mapping and inode-context release.
        names[8] = 'cached_read_total'
        descriptors[8]=('nvfuse_indirect.c','nvfuse_try_cached_read_mapping','sb, ino, offset, bytes, physical',False)
    for number,(filename,name,args,is_void) in enumerate(descriptors):
        p=output/filename;s=p.read_text()
        match=re.search(r'(?m)^([^\n;{}]*\b'+re.escape(name)+r'\([^;{}]*\))\n\{',s)
        if not match:raise ValueError('missing function definition: '+name)
        signature=match.group(1);impl=signature.replace(name+'(',name+'_profile_impl(',1)
        s=s[:match.start(1)]+impl+s[match.end(1):]
        return_type=signature[:signature.index(name)].strip()
        invoke=f'{name}_profile_impl({args})'
        call=(invoke+';') if is_void else (return_type+' result = '+invoke+';')
        s+='\n'+signature+'\n{\n    uint64_t *detail = nvfuse_hot_profile;\n    uint64_t begin = detail ? spdk_get_ticks() : 0;\n    '+call+f'''\n    if (detail) {{ detail[{number*2}] += spdk_get_ticks() - begin; detail[{number*2+1}]++; }}\n'''+('' if is_void else '    return result;\n')+'}\n'
        if '#include "nvfuse_hot_profile.h"' not in s:
            s=s.replace('#include "nvfuse_core.h"','#include "nvfuse_core.h"\n#include "nvfuse_hot_profile.h"',1)
        p.write_text(s)
    p=output/'nvfuse_aio.c';s=p.read_text().replace('#include "nvfuse_core.h"','#include "nvfuse_core.h"\n#include "nvfuse_hot_profile.h"\n_Thread_local uint64_t *nvfuse_hot_profile;',1);p.write_text(s)
    p=output/'examples/fio_plugin/modern/runtime.c';s=p.read_text().replace('#include "spsc.h"','#include "spsc.h"\n#include "nvfuse_hot_profile.h"')
    s=s.replace('uint64_t profile_sum[2][8], profile_max[2][8];','uint64_t profile_sum[2][8], profile_max[2][8], detail_sum[2][20];')
    s=s.replace('struct nvfio_request { uint64_t stage_tsc[9];','struct nvfio_request { uint64_t detail[20]; uint64_t stage_tsc[9];')
    s=s.replace('    int rc = nvfuse_aio_queue_submission(rt.handle, &lane->queue, &r->aio);', '''    nvfuse_hot_profile = r->aio.stage_tsc ? r->detail : NULL;
    int rc = nvfuse_aio_queue_submission(rt.handle, &lane->queue, &r->aio);
    nvfuse_hot_profile = NULL;''')
    s=s.replace('memset(r->stage_tsc, 0, sizeof(r->stage_tsc));','memset(r->stage_tsc, 0, sizeof(r->stage_tsc));\n                memset(r->detail, 0, sizeof(r->detail));')
    s=s.replace('                client->profile_count[op]++;','                client->profile_count[op]++;\n                for (unsigned i = 0; i < 20; i++) client->detail_sum[op][i] += r->detail[i];')
    anchor='            puts("");\n        }\n    }'
    assert s.count(anchor)==1
    encoded=', '.join('"'+n+'"' for n in names)
    s=s.replace(anchor,'''            puts("");
            static const char *details[] = {'''+encoded+'''};
            printf("NVFUSE_DETAIL opcode=%s count=%" PRIu64, op ? "write" : "read", count);
            for (unsigned i = 0; i < 10; i++)
                printf(" %s_ns=%.3f %s_calls=%.6f", details[i],
                    count ? (double)c->client->detail_sum[op][2*i] * 1e9 / spdk_get_ticks_hz() / count : 0,
                    details[i], count ? (double)c->client->detail_sum[op][2*i+1] / count : 0);
            puts("");
        }
    }''')
    p.write_text(s)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();instrument_hotpath(args.source,args.output)
