#!/usr/bin/env python3
"""Copy the primary source and add nested metadata-function elapsed timers.

Diagnostic executable only: rebuild its archive and metadata_bench together.
The source tree and production ABI are never modified by this generator.
NVMETA_DETAIL=1 enables sampling; NVMETA_SAMPLE=all also captures the first op.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

FUNCTIONS = {
    'nvfuse_api.c': ['nvfuse_openfile_path', 'nvfuse_openfile', 'nvfuse_closefile',
        'nvfuse_path_resolve', 'nvfuse_lookup', 'nvfuse_lookup_bptree',
        'nvfuse_lookup_linear', 'nvfuse_set_directory_parent', 'nvfuse_createfile',
        'nvfuse_make_first_directory', 'nvfuse_mkdir_path', 'nvfuse_mkdir',
        'nvfuse_rename_path', 'nvfuse_unlink', 'nvfuse_rmdir_path'],
    'nvfuse_core.c': ['nvfuse_alloc_new_inode', 'nvfuse_find_free_inode',
        'nvfuse_scan_free_ibitmap', 'nvfuse_find_empty_dentry', 'nvfuse_read_inode',
        'nvfuse_release_inode', 'nvfuse_set_dir_indexing', 'nvfuse_get_dir_indexing',
        'nvfuse_check_flush_dirty', 'nvfuse_create_bptree', 'nvfuse_get_free_blocks',
        'nvfuse_alloc_dbitmap'],
    'nvfuse_indirect.c': ['nvfuse_get_block', 'nvfuse_alloc_free_block'],
    'nvfuse_inode_cache.c': ['nvfuse_alloc_ictx', 'nvfuse_get_ictx'],
    'nvfuse_buffer_cache.c': ['_nvfuse_get_bh', 'nvfuse_alloc_buffer_head',
        'nvfuse_get_bc', 'nvfuse_hash_lookup', 'nvfuse_release_bh'],
    'nvfuse_bp_tree.c': ['bp_find_key', 'bp_insert_key_tree', 'bp_alloc_inode_and_master'],
    'nvfuse_reactor.c': ['reactor_sync_read_blk', 'reactor_sync_write_blk'],
}


def instrument(source, output):
    source, output = source.resolve(), output.resolve()
    if output.exists():
        raise ValueError('output directory must be new')
    files = list(source.glob('*.c')) + [source / 'Makefile.spdk']
    files += list((source / 'include').glob('*.h'))
    files += list(source.glob('*.inc')) + list((source / 'spdk_runtime').glob('*'))
    files += [p for p in (source / 'examples/fio_plugin/modern').iterdir()
              if p.suffix in ('.c', '.h', '.map') or p.name == 'Makefile']
    files += [source / 'tests/spdk' / n for n in
              ['metadata_bench.c', 'Makefile.tools']]
    manifest = {}
    for p in files:
        rel = p.relative_to(source); dest = output / rel
        dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(p, dest)
        manifest[str(rel)] = hashlib.sha256(p.read_bytes()).hexdigest()
    names = [n for group in FUNCTIONS.values() for n in group]
    header = r'''#ifndef NVMETA_PROFILE_H
#define NVMETA_PROFILE_H
#include <stdint.h>
#include <time.h>
#include <errno.h>
struct nvmeta_frame { struct nvmeta_frame *parent; uint64_t start, children; };
struct nvmeta_counter { uint64_t calls, inclusive_ns, exclusive_ns; };
extern _Thread_local int nvmeta_active;
extern _Thread_local struct nvmeta_frame *nvmeta_parent;
extern _Thread_local struct nvmeta_counter nvmeta_counter[NVMETA_FUNCTIONS];
static inline uint64_t nvmeta_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
static inline void nvmeta_enter(struct nvmeta_frame *f) {
    f->parent = nvmeta_parent; f->children = 0; nvmeta_parent = f; f->start = nvmeta_now();
}
static inline void nvmeta_leave(unsigned id, struct nvmeta_frame *f) {
    uint64_t elapsed = nvmeta_now() - f->start;
    nvmeta_counter[id].calls++; nvmeta_counter[id].inclusive_ns += elapsed;
    nvmeta_counter[id].exclusive_ns += elapsed - f->children;
    nvmeta_parent = f->parent;
    if (f->parent) f->parent->children += elapsed;
}
#endif
'''.replace('#include <stdint.h>', '#include <stdint.h>\n#define NVMETA_FUNCTIONS '+str(len(names)))
    (output / 'include/nvmeta_profile.h').write_text(header)
    for filename, functions in FUNCTIONS.items():
        p = output / filename; text = p.read_text()
        for name in functions:
            matches = list(re.finditer(r'(?m)^([^\n;{}]*\b'+re.escape(name)+r'\([^;{}]*\))\s*\n\{', text))
            if len(matches) != 1:
                raise ValueError(f'{filename}: expected one function definition: {name}, got {len(matches)}')
            m = matches[0]; signature = m[1]; impl = signature.replace(name+'(', name+'_nvmeta_impl(', 1)
            parameters = signature[signature.index('(')+1:signature.rindex(')')]
            args = []
            if parameters.strip() != 'void':
                for param in parameters.split(','):
                    param = re.sub(r'\[[^]]*\]', '', param).strip()
                    if '(' in param or ')' in param:
                        raise ValueError('unsupported function-pointer parameter: '+param)
                    args.append(re.search(r'(\w+)\s*$', param)[1])
            invoke = name+'_nvmeta_impl('+', '.join(args)+')'
            return_type = signature[:signature.index(name)].strip()
            return_type = re.sub(r'\b(static|inline)\b', '', return_type).strip()
            is_void = return_type == 'void'
            fast = invoke+'; return;' if is_void else 'return '+invoke+';'
            call = invoke+';' if is_void else return_type+' result = '+invoke+';'
            wrapper = (impl+';\n'+signature+'\n{\n'
                '    if (!nvmeta_active) { '+fast+' }\n'
                '    struct nvmeta_frame frame; nvmeta_enter(&frame);\n'
                '    '+call+'\n    int saved_errno = errno;\n'
                '    nvmeta_leave('+str(names.index(name))+', &frame); errno = saved_errno;\n'
                +('' if is_void else '    return result;\n')+'}\n'+impl)
            text = text[:m.start(1)] + wrapper + text[m.end(1):]
        p.write_text('#include "nvmeta_profile.h"\n'+text)
    p = output / 'tests/spdk/metadata_bench.c'; text = p.read_text()
    definitions = r'''
#ifndef NVMETA_POSIX
#include "nvmeta_profile.h"
_Thread_local int nvmeta_active;
_Thread_local struct nvmeta_frame *nvmeta_parent;
_Thread_local struct nvmeta_counter nvmeta_counter[NVMETA_FUNCTIONS];
static uint64_t nvmeta_samples, nvmeta_sample_ns;
static uint64_t nvmeta_first_ns;
static struct nvmeta_counter nvmeta_first[NVMETA_FUNCTIONS];
static int nvmeta_enabled, nvmeta_full;
static const char *nvmeta_names[] = NVMETA_NAMES;
static void nvmeta_reset(void) {
    memset(nvmeta_counter, 0, sizeof(nvmeta_counter));
    nvmeta_samples = nvmeta_sample_ns = nvmeta_first_ns = 0; nvmeta_parent = NULL;
}
static void nvmeta_report(const char *phase) {
    if (!nvmeta_enabled) return;
    printf("NVMETA_DETAIL {\"phase\":\"%s\",\"sampled_operations\":%" PRIu64
           ",\"sample_ns\":%" PRIu64 ",\"functions\":[", phase, nvmeta_samples, nvmeta_sample_ns);
    for (unsigned i = 0; i < NVMETA_FUNCTIONS; i++)
        printf("%s{\"name\":\"%s\",\"calls\":%" PRIu64 ",\"inclusive_ns\":%" PRIu64
               ",\"exclusive_ns\":%" PRIu64 "}", i ? "," : "", nvmeta_names[i],
               nvmeta_counter[i].calls, nvmeta_counter[i].inclusive_ns, nvmeta_counter[i].exclusive_ns);
    puts("]}");
    if (nvmeta_first_ns) {
        printf("NVMETA_FIRST {\"phase\":\"%s\",\"operation_ns\":%" PRIu64
               ",\"functions\":[", phase, nvmeta_first_ns);
        for (unsigned i = 0; i < NVMETA_FUNCTIONS; i++)
            printf("%s{\"name\":\"%s\",\"calls\":%" PRIu64 ",\"inclusive_ns\":%" PRIu64
                   ",\"exclusive_ns\":%" PRIu64 "}", i ? "," : "", nvmeta_names[i],
                   nvmeta_first[i].calls, nvmeta_first[i].inclusive_ns, nvmeta_first[i].exclusive_ns);
        puts("]}");
    }
}
#endif
'''.replace('NVMETA_NAMES', '{'+', '.join(json.dumps(n) for n in names)+'}')
    text = text.replace('struct workload {', definitions+'\nstruct workload {', 1)
    text = text.replace('    fflush(stdout);\n}', '''#ifndef NVMETA_POSIX
    nvmeta_report(name);
#endif
    fflush(stdout);
}''', 1)
    text = text.replace('    uint64_t begin = ns(), sync_time = 0;', '''#ifndef NVMETA_POSIX
    nvmeta_reset();
#endif
    uint64_t begin = ns(), sync_time = 0;''', 1)
    text = text.replace('        if (operation(w, op, a, b)) {', '''#ifndef NVMETA_POSIX
        uint64_t sample = (uint64_t)i + (uint64_t)r * count + 1;
        sample = (sample ^ (sample >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        sample = (sample ^ (sample >> 27)) * UINT64_C(0x94d049bb133111eb);
        int sampled = nvmeta_enabled && (nvmeta_full || !((sample ^ (sample >> 31)) & 63));
        uint64_t sample_start = sampled ? ns() : 0;
        nvmeta_active = sampled;
#endif
        int operation_rc = operation(w, op, a, b);
#ifndef NVMETA_POSIX
        nvmeta_active = 0;
        if (sampled) {
            uint64_t elapsed = ns() - sample_start;
            nvmeta_samples++; nvmeta_sample_ns += elapsed;
            if (!i && !r) {
                nvmeta_first_ns = elapsed;
                memcpy(nvmeta_first, nvmeta_counter, sizeof(nvmeta_first));
            }
        }
#endif
        if (operation_rc) {''', 1)
    text = text.replace('    uint64_t begin = ns(); unsigned count = 0;', '''#ifndef NVMETA_POSIX
    nvmeta_reset();
#endif
    uint64_t begin = ns(); unsigned count = 0;''', 1)
    text = text.replace('    struct workload w = {.count', '''#ifndef NVMETA_POSIX
    nvmeta_enabled = getenv("NVMETA_DETAIL") && !strcmp(getenv("NVMETA_DETAIL"), "1");
    const char *sample_mode = getenv("NVMETA_SAMPLE");
    if (sample_mode && strcmp(sample_mode, "all") && strcmp(sample_mode, "64")) return 2;
    nvmeta_full = sample_mode && !strcmp(sample_mode, "all");
#endif
    struct workload w = {.count''', 1)
    p.write_text(text)
    (output / 'metadata-profile-source-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (output / 'metadata-profile-functions.json').write_text(json.dumps(names, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); instrument(args.source, args.output)
