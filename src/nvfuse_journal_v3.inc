/* SPDX-License-Identifier: GPL-2.0-only */

/* Experimental v3, included by the engine to share endian/CRC/I/O helpers.
 * Append-only within an epoch. A complete checksummed record plus a completed
 * flush commits a transaction. Home writes occur only at epoch reclamation.
 * Both epoch controls are persisted before any old log bytes can be reused.
 * v1/v2 encoding and default format selection are unchanged. */
#define V3_CONTROL UINT64_C(0x334c52544346564e)
#define V3_RECORD UINT64_C(0x334345524a46564e)
#define V3_HEADER 128u
#define V3_CRC 60u
#define V3_CHUNK 128u
struct nvj_v3 {
    uint64_t epoch, sequence;
    unsigned cursor, count;
    uint32_t previous;
    struct sparse index;
    struct entry *entries;
    unsigned char *images, *record;
};
static size_t v3_memory_bytes(const struct nvj *j) {
    return j->v3 ? sizeof(*j->v3)+(size_t)j->v3->index.capacity*sizeof(struct bucket)+
        (size_t)j->capacity*(NVJ_BLOCK+sizeof(struct entry))+(size_t)(j->blocks-3)*NVJ_BLOCK:0;
}
static const unsigned char *v3_image(struct nvj *j,uint32_t block) {
    if(!j->v3 || !j->v3->index.used) return NULL;
    unsigned n=lookup(&j->v3->index,block)->value;
    return n?j->v3->images+(size_t)(n-1)*NVJ_BLOCK:NULL;
}
static int v3_controls(struct nvj *j,uint64_t epoch) {
    unsigned char b[NVJ_BLOCK];
    memset(b,0,sizeof(b)); put64(b,V3_CONTROL); put64(b+8,epoch);
    memcpy(b+16,j->uuid,16); seal(b);
    for(unsigned i=1;i<=2;i++) {
        int rc=transfer(j,log_block(j,i),b,1,"v3-epoch-write");
        if(!rc) rc=barrier(j,"v3-epoch-flush");
        if(rc) return rc;
        j->stats.journal_bytes+=NVJ_BLOCK;
    }
    return 0;
}
static int v3_format(struct nvj *j) { return v3_controls(j,1); }
static int v3_read_log(struct nvj *j,unsigned slot,unsigned count,unsigned char *buf) {
    for(unsigned i=0;i<count;) {
        unsigned n=1; uint32_t block=log_block(j,slot+i);
        while(n<run_limit(j) && i+n<count && log_block(j,slot+i+n)==block+n) n++;
        int rc=n==1?transfer(j,block,buf+(size_t)i*NVJ_BLOCK,0,"v3-log-read"):
            physical(j,(uint64_t)block*NVJ_BLOCK,(size_t)n*NVJ_BLOCK,buf+(size_t)i*NVJ_BLOCK,0);
        if(rc) return rc;
        i+=n;
    }
    return 0;
}
static int v3_home(struct nvj *j) {
    struct nvj_v3 *v=j->v3;
    for(unsigned i=0;i<v->count;) {
        if(j->home_writer && j->batch_io && !j->hook && !j->fault) {
            struct nvj_home_write w[NVJ_HOME_BATCH_BLOCKS];
            unsigned end=i+NVJ_HOME_BATCH_BLOCKS,count=0;
            if(end>v->count) end=v->count;
            while(i<end) {
                unsigned n=1;
                while(i+n<end && v->entries[i+n].home==v->entries[i].home+n) n++;
                w[count++]=(struct nvj_home_write){(uint64_t)v->entries[i].home*NVJ_BLOCK,
                    (size_t)n*NVJ_BLOCK,v->images+(size_t)i*NVJ_BLOCK}; i+=n;
            }
            int rc=j->home_writer(j->home_context,w,count);
            j->stats.home_batches++; j->stats.home_requests+=count;
            if(rc) return rc;
        } else {
            unsigned n=1;
            while(n<run_limit(j) && i+n<v->count && v->entries[i+n].home==v->entries[i].home+n) n++;
            int rc=write_run(j,v->entries[i].home,n,v->images+(size_t)i*NVJ_BLOCK,"v3-home-write");
            if(rc) return rc;
            i+=n;
        }
    }
    return 0;
}
static int v3_checkpoint(struct nvj *j) {
    struct nvj_v3 *v=j->v3;
    if(v->epoch==UINT64_MAX) return -EOVERFLOW;
    uint64_t start=profile_start(j);
    int rc=v3_home(j);
    profile_end(j,NVJ_P_HOME_WRITE,start); start=profile_start(j);
    if(!rc) rc=barrier(j,"v3-home-flush");
    profile_end(j,NVJ_P_HOME_FLUSH,start); start=profile_start(j);
    if(!rc) rc=v3_controls(j,v->epoch+1);
    profile_end(j,NVJ_P_RETIRE,start);
    if(rc) { j->failed=1; return rc; }
    j->stats.home_bytes+=(uint64_t)v->count*NVJ_BLOCK;
    v->epoch++; v->sequence=0; v->previous=0; v->cursor=3; v->count=0;
    memset(v->index.buckets,0,(size_t)v->index.capacity*sizeof(*v->index.buckets));
    v->index.used=0;
    event(j,"v3-reclaimed",v->epoch);
    return 0;
}
static void v3_close(struct nvj *j) {
    if(!j->v3) return;
    free(j->v3->index.buckets); free(j->v3->entries); free(j->v3->images);
    free(j->v3->record); free(j->v3); j->v3=NULL;
}
/* Validate a complete record before changing the committed overlay. Recovery
 * does not touch home until the entire valid append prefix has been checked. */
static int v3_decode(struct nvj *j,unsigned bytes) {
    struct nvj_v3 *v=j->v3; unsigned char *b=v->record;
    unsigned used=get32(b+48),count=get32(b+52),full=get32(b+56),at=V3_HEADER;
    if(used<V3_HEADER || used>bytes || !count || count>j->capacity || full>1) return -EINVAL;
    memset(j->index.buckets,0,(size_t)j->index.capacity*sizeof(*j->index.buckets)); j->index.used=0;
    unsigned new_count=0;
    for(unsigned i=0;i<count;i++) {
        if(used-at<16) return -EINVAL;
        uint32_t home=get32(b+at),kind=get32(b+at+4),mask=get32(b+at+8);
        if(home>=j->bytes/NVJ_BLOCK || (home>=j->start && home<NVJ_GROUP_BLOCKS) ||
           kind>1 || (!full && !kind) || !mask || get32(b+at+12) || lookup(&j->index,home)->value) return -EINVAL;
        int rc=remember(&j->index,home,1); if(rc) return rc;
        unsigned n=(unsigned)__builtin_popcount(mask)*V3_CHUNK;
        at+=16; if(n>used-at) return -EINVAL;
        if(!lookup(&v->index,home)->value) { if(mask!=UINT32_MAX) return -EINVAL; new_count++; }
        at+=n;
    }
    if(at!=used || new_count>j->capacity-v->count) return -EINVAL;
    at=V3_HEADER;
    for(unsigned i=0;i<count;i++) {
        uint32_t home=get32(b+at),kind=get32(b+at+4),mask=get32(b+at+8); at+=16;
        unsigned n=lookup(&v->index,home)->value;
        if(!n) {
            n=++v->count; int rc=remember(&v->index,home,n); if(rc) return rc;
            v->entries[n-1]=(struct entry){.home=home,.kind=kind};
        }
        for(unsigned k=0;k<32;k++) if(mask&(UINT32_C(1)<<k)) {
            memcpy(v->images+(size_t)(n-1)*NVJ_BLOCK+k*V3_CHUNK,b+at,V3_CHUNK); at+=V3_CHUNK;
        }
    }
    memset(j->index.buckets,0,(size_t)j->index.capacity*sizeof(*j->index.buckets)); j->index.used=0;
    return 0;
}
static int v3_open(struct nvj *j) {
    struct nvj_v3 *v=calloc(1,sizeof(*v)); if(!v) return -ENOMEM; j->v3=v;
    v->entries=calloc(j->capacity,sizeof(*v->entries));
    v->images=malloc((size_t)j->capacity*NVJ_BLOCK);
    v->record=malloc((size_t)(j->blocks-3)*NVJ_BLOCK);
    int rc=reserve(&v->index,j->index.capacity);
    if(rc || !v->entries || !v->images || !v->record) return -ENOMEM;
    unsigned char c[NVJ_BLOCK];
    for(unsigned i=1;i<=2;i++) {
        rc=transfer(j,log_block(j,i),c,0,"v3-epoch-read"); if(rc) return rc;
        if(valid(c,V3_CONTROL) && !memcmp(c+16,j->uuid,16) && get64(c+8)>v->epoch) v->epoch=get64(c+8);
    }
    if(!v->epoch) return -EINVAL;
    v->cursor=3;
    while(v->cursor<j->blocks) {
        unsigned char *b=v->record;
        rc=v3_read_log(j,v->cursor,1,b); if(rc) return rc;
        if(get64(b)!=V3_RECORD || get64(b+8)!=v->epoch || memcmp(b+16,j->uuid,16)) break;
        unsigned blocks=get32(b+44);
        if(!blocks || blocks>j->blocks-v->cursor) break; /* Torn append tail. */
        if(blocks>1) { rc=v3_read_log(j,v->cursor+1,blocks-1,b+NVJ_BLOCK); if(rc) return rc; }
        uint32_t checksum=get32(b+V3_CRC); put32(b+V3_CRC,0);
        uint32_t actual=crc(b,(size_t)blocks*NVJ_BLOCK); put32(b+V3_CRC,checksum);
        if(actual!=checksum) break;
        if(v->sequence==UINT64_MAX || get64(b+32)!=v->sequence+1 || get32(b+40)!=v->previous) return -EINVAL;
        rc=v3_decode(j,blocks*NVJ_BLOCK); if(rc) return rc;
        v->sequence++; v->previous=checksum; v->cursor+=blocks; j->stats.replays++;
    }
    /* Even an incomplete tail must be fenced by a new epoch before reuse. */
    return v3_checkpoint(j);
}
static uint32_t v3_mask(const unsigned char *image,const unsigned char *old) {
    if(!old) return UINT32_MAX;
    uint32_t mask=0;
    for(unsigned k=0;k<32;k++) if(memcmp(image+k*V3_CHUNK,old+k*V3_CHUNK,V3_CHUNK)) mask|=UINT32_C(1)<<k;
    return mask;
}
static int v3_commit(struct nvj *j) {
    struct nvj_v3 *v=j->v3;
    if(!j->count) {
        int rc=j->data_dirty?barrier(j,"ordered-data-flush"):0;
        if(rc) j->failed=1; else discard(j);
        return rc;
    }
    /* Reserve using full images BEFORE encoding. Checkpoint old committed
     * images only; active images stay isolated in the ordinary TX overlay. */
    size_t maximum=V3_HEADER+(size_t)j->count*(16+NVJ_BLOCK);
    unsigned max_blocks=(unsigned)((maximum+NVJ_BLOCK-1)/NVJ_BLOCK);
    if(max_blocks>j->blocks-3) return -E2BIG;
    if(max_blocks>j->blocks-v->cursor || j->count>j->capacity-v->count) {
        int rc=v3_checkpoint(j); if(rc) return rc;
    }
    if(v->sequence==UINT64_MAX) return -EOVERFLOW;
    uint64_t start=profile_start(j);
    unsigned char *b=v->record; memset(b,0,V3_HEADER);
    unsigned at=V3_HEADER,count=0;
    for(unsigned i=0;i<j->count;i++) {
        struct entry *e=&j->entries[i]; unsigned char *image=j->images+(size_t)i*NVJ_BLOCK;
        uint32_t mask=v3_mask(image,v3_image(j,e->home)); if(!mask) continue;
        put32(b+at,e->home); put32(b+at+4,e->kind); put32(b+at+8,mask); put32(b+at+12,0); at+=16;
        for(unsigned k=0;k<32;k++) if(mask&(UINT32_C(1)<<k)) { memcpy(b+at,image+k*V3_CHUNK,V3_CHUNK); at+=V3_CHUNK; }
        count++;
    }
    unsigned blocks=(at+NVJ_BLOCK-1)/NVJ_BLOCK;
    memset(b+at,0,(size_t)blocks*NVJ_BLOCK-at);
    put64(b,V3_RECORD); put64(b+8,v->epoch); memcpy(b+16,j->uuid,16);
    put64(b+32,v->sequence+1); put32(b+40,v->previous); put32(b+44,blocks);
    put32(b+48,at); put32(b+52,count); put32(b+56,(unsigned)j->full);
    /* Previously protected full-data images stay overlaid until reclamation,
     * even if the next transaction requests metadata-only protection. */
    for(unsigned i=0;i<j->count;i++) if(!j->entries[i].kind) { put32(b+56,1); break; }
    uint32_t checksum=crc(b,(size_t)blocks*NVJ_BLOCK); put32(b+V3_CRC,checksum);
    profile_end(j,NVJ_P_DESCRIPTOR,start); start=profile_start(j);
    int rc=j->data_dirty?barrier(j,"ordered-data-flush"):0;
    profile_end(j,NVJ_P_ORDERED,start); start=profile_start(j);
    if(!rc && count) rc=write_log(j,v->cursor,blocks,b,"v3-log-write");
    profile_end(j,NVJ_P_LOG_WRITE,start); start=profile_start(j);
    if(!rc && count) rc=barrier(j,"v3-log-flush");
    profile_end(j,NVJ_P_PAYLOAD_FLUSH,start);
    if(rc) { j->failed=1; return rc; }
    start=profile_start(j);
    if(count) {
        /* All storage is reserved at open, so acknowledgement cannot depend
         * on an allocation after durable publication. */
        for(unsigned i=0;i<j->count;i++) {
            struct entry *e=&j->entries[i]; unsigned n=lookup(&v->index,e->home)->value;
            if(!n) { n=++v->count; rc=remember(&v->index,e->home,n); if(rc) { j->failed=1; return rc; } v->entries[n-1]=*e; }
            memcpy(v->images+(size_t)(n-1)*NVJ_BLOCK,j->images+(size_t)i*NVJ_BLOCK,NVJ_BLOCK);
        }
        v->sequence++; v->previous=checksum; v->cursor+=blocks;
        j->stats.commits++; j->stats.journal_bytes+=(uint64_t)blocks*NVJ_BLOCK;
        event(j,"v3-durable",v->sequence);
    }
    profile_end(j,NVJ_P_CERTIFICATE,start); start=profile_start(j);
    discard(j);
    profile_end(j,NVJ_P_PREPARE,start); return 0;
}
