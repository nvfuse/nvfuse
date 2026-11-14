/* SPDX-License-Identifier: GPL-2.0-only */

/* Exercise the real list/hash transitions, including colliding inode keys. */
#include "nvfuse_core.h"
#include "nvfuse_inode_cache.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void validate(struct nvfuse_ictx_manager *m, unsigned expected)
{
    unsigned lists = 0, hashes = 0;
    for (unsigned type = 0; type < BUFFER_TYPE_NUM; type++) {
        struct nvfuse_inode_ctx *c;
        unsigned count = 0;
        list_for_each_entry(c, &m->ictxc_list[type], ictx_cache_list) {
            CHECK(c->ictx_type == (int)type);
            count++;
        }
        CHECK((int)count == m->ictxc_list_count[type]);
        lists += count;
    }
    for (unsigned bucket = 0; bucket <= HASH_NUM; bucket++) {
        struct hlist_node *node;
        unsigned count = 0;
        hlist_for_each(node, &m->ictxc_hash[bucket]) {
            struct nvfuse_inode_ctx *c = hlist_entry(node, struct nvfuse_inode_ctx, ictx_hash);
            CHECK(bucket == (c->ictx_type == BUFFER_TYPE_UNUSED ? HASH_NUM : c->ictx_ino % HASH_NUM));
            count++;
        }
        CHECK((int)count == m->ictxc_hash_count[bucket]);
        hashes += count;
    }
    CHECK(lists == expected && hashes == expected);
}

int main(void)
{
    struct nvfuse_superblock sb = {0};
    struct nvfuse_ictx_manager *m = calloc(1, sizeof(*m));
    struct nvfuse_inode_ctx entries[3];
    CHECK(m);
    sb.sb_ictxc = m;
    SPINLOCK_INIT(&m->ictxc_lock);
    for (unsigned i = 0; i < BUFFER_TYPE_NUM; i++) INIT_LIST_HEAD(&m->ictxc_list[i]);
    for (unsigned i = 0; i <= HASH_NUM; i++) INIT_HLIST_HEAD(&m->ictxc_hash[i]);
    for (unsigned i = 0; i < 3; i++) {
        nvfuse_init_ictx(&entries[i], i == 1 ? ROOT_INO + HASH_NUM : ROOT_INO + i);
        nvfuse_insert_ictx(&sb, &entries[i]);
    }
    validate(m, 3);
    for (unsigned i = 0; i < 200000; i++) {
        struct nvfuse_inode_ctx *c = &entries[i % 3];
        const int states[] = {BUFFER_TYPE_CLEAN, BUFFER_TYPE_CLEAN, BUFFER_TYPE_DIRTY,
                              BUFFER_TYPE_DIRTY, BUFFER_TYPE_REF, BUFFER_TYPE_UNUSED};
        for (unsigned j = 0; j < sizeof(states) / sizeof(*states); j++) {
            nvfuse_move_ictx_list(&sb, c, states[j]);
            CHECK(m->ictxc_list[states[j]].next == &c->ictx_cache_list);
            CHECK(nvfuse_ictx_hash_lookup(m, c->ictx_ino) ==
                  (states[j] == BUFFER_TYPE_UNUSED ? NULL : c));
        }
        if (!(i % 10000)) validate(m, 3);
    }
    for (unsigned i = 0; i < 3; i++) nvfuse_move_ictx_list(&sb, &entries[i], BUFFER_TYPE_CLEAN);
    for (unsigned i = 0; i < 200000; i++) {
        struct nvfuse_inode_ctx *c = nvfuse_get_ictx(&sb, entries[i % 3].ictx_ino);
        CHECK(c == &entries[i % 3]);
        c->ictx_ref++;
        nvfuse_release_ictx(&sb, c, NVF_CLEAN);
        CHECK(!c->ictx_ref);
    }
    validate(m, 3);
    free(m);
    puts("PASS inode cache: 1.2M state transitions, hash collisions, exact counts and 200k acquire/release pairs");
    return 0;
}
