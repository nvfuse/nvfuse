/* Single-writer redo journal. The disk ABI uses explicit little-endian fields.
 * Home writes follow TWO independently flushed commit certificates. Controls
 * are retired in both copies before the next slot can overwrite payload. */
#include "nvfuse_journal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define ANCHOR UINT64_C(0x314c4e524a46564e)
#define CONTROL UINT64_C(0x314c52544346564e)
#define COMMIT UINT64_C(0x31544d4f4346564e)
#define DESCRIPTOR UINT64_C(0x314353454446564e)
#define VERSION 1u
#define RETIRED 1u
#define PREPARED 0u
#define HEADER 128u
#define CHECKSUM (NVJ_BLOCK - 4u)
struct entry { uint32_t home, kind, checksum; };
struct nvj {
    struct nvj_ops ops;
    uint64_t bytes, id;
    unsigned char uuid[16];
    unsigned char *metadata, *images;
    uint32_t *index;
    struct entry *entries;
    unsigned allocated;
    unsigned count, budget;
    int active, full, error, failed;
    struct nvj_stats stats;
    nvj_hook hook;
    void *hook_arg;
};
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t get64(const unsigned char *p) { return get32(p) | (uint64_t)get32(p+4)<<32; }
static void put32(unsigned char *p, uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(n>>(8*i)); }
static void put64(unsigned char *p, uint64_t n) { put32(p,(uint32_t)n); put32(p+4,(uint32_t)(n>>32)); }
static uint32_t crc(const void *buf, size_t len) {
    const unsigned char *p=buf;
    uint32_t c=~0u;
    while(len--) { c^=*p++; for(unsigned i=0;i<8;i++) c=(c>>1)^(0x82f63b78u & (0u-(c&1u))); }
    return ~c;
}
static void seal(unsigned char *b) { put32(b+CHECKSUM,crc(b,CHECKSUM)); }
static int valid(const unsigned char *b,uint64_t magic) { return get64(b)==magic && get32(b+CHECKSUM)==crc(b,CHECKSUM); }
static void event(struct nvj *j,const char *name,uint64_t block) { if(j->hook) j->hook(j->hook_arg,name,block); }
static int transfer(struct nvj *j,uint32_t b,void *p,int write_op,const char *phase) {
    event(j,"before-io",b);
    int rc=j->ops.transfer(j->ops.context,(uint64_t)b*NVJ_BLOCK,NVJ_BLOCK,p,write_op);
    if(!rc) event(j,phase,b);
    return rc;
}
static int barrier(struct nvj *j,const char *phase) {
    int rc=j->ops.flush(j->ops.context);
    if(!rc) { j->stats.flushes++; event(j,phase,0); }
    return rc;
}
static void identity(struct nvj *j,unsigned char *b,uint64_t magic) {
    memset(b,0,NVJ_BLOCK); put64(b,magic); put64(b+8,j->id); memcpy(b+16,j->uuid,16);
}
static int same(struct nvj *j,const unsigned char *b) { return get64(b+8)==j->id && !memcmp(b+16,j->uuid,16); }
static int controls(struct nvj *j,unsigned state) {
    unsigned char b[NVJ_BLOCK]; identity(j,b,CONTROL); put32(b+32,state); seal(b);
    for(unsigned i=1;i<=2;i++) {
        int rc=transfer(j,NVJ_START+i,b,1,"control-write"); if(rc) return rc;
        rc=barrier(j,state==RETIRED?"retire-flush":"prepare-flush"); if(rc) return rc;
    }
    return 0;
}
static int replay(struct nvj *j,const unsigned char *cert) {
    unsigned n=get32(cert+32);
    if(!n || n>NVJ_CAPACITY || get32(cert+40)>1) return -EINVAL;
    unsigned char *d=malloc(NVJ_DESCRIPTORS*NVJ_BLOCK);
    unsigned char *images=malloc((size_t)n*NVJ_BLOCK);
    if(!d || !images) { free(d); free(images); return -ENOMEM; }
    int rc=0;
    for(unsigned i=0;i<NVJ_DESCRIPTORS && !rc;i++) rc=transfer(j,NVJ_START+3+i,d+i*NVJ_BLOCK,0,"descriptor-read");
    if(!rc && (get64(d)!=DESCRIPTOR || !same(j,d) || get32(d+32)!=n ||
       get32(d+36)!=get32(cert+40) || crc(d,NVJ_DESCRIPTORS*NVJ_BLOCK)!=get32(cert+36))) rc=-EINVAL;
    /* Validate EVERY payload and destination before any replay home write. */
    for(unsigned i=0;i<n && !rc;i++) {
        unsigned char *e=d+HEADER+i*16;
        uint32_t home=get32(e),kind=get32(e+4);
        if(home>=j->bytes/NVJ_BLOCK || (home>=NVJ_START && home<NVJ_START+NVJ_BLOCKS) ||
           kind>1 || (!get32(cert+40) && !kind)) { rc=-EINVAL; break; }
        for(unsigned k=0;k<i;k++) if(get32(d+HEADER+k*16)==home) rc=-EINVAL;
        if(rc) break;
        rc=transfer(j,NVJ_START+NVJ_PAYLOAD+i,images+(size_t)i*NVJ_BLOCK,0,"payload-read");
        if(!rc && crc(images+(size_t)i*NVJ_BLOCK,NVJ_BLOCK)!=get32(e+8)) rc=-EINVAL;
    }
    for(unsigned i=0;i<n && !rc;i++) {
        rc=transfer(j,get32(d+HEADER+i*16),images+(size_t)i*NVJ_BLOCK,1,"replay-home");
        if(!rc) j->stats.home_bytes+=NVJ_BLOCK;
    }
    if(!rc) rc=barrier(j,"replay-flush");
    if(!rc) rc=controls(j,RETIRED);
    if(!rc) j->stats.replays++;
    free(d); free(images); return rc;
}
int nvj_format(const struct nvj_ops *ops,uint64_t bytes,int metadata_only) {
    if(bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK || bytes/NVJ_BLOCK>UINT32_MAX ||
       bytes%((uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK) || (metadata_only!=0 && metadata_only!=1)) return -EINVAL;
    struct nvj j={.ops=*ops,.bytes=bytes};
    int random_fd=open("/dev/urandom",O_RDONLY);
    if(random_fd<0) return -errno;
    size_t obtained=0;
    while(obtained<sizeof(j.uuid)) {
        ssize_t n=read(random_fd,j.uuid+obtained,sizeof(j.uuid)-obtained);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { int error=n<0?-errno:-EIO; close(random_fd); return error; }
        obtained+=(size_t)n;
    }
    close(random_fd);
    unsigned char b[NVJ_BLOCK]={0};
    /* Old certificates may remain only with a different volume UUID. */
    for(unsigned i=NVJ_BLOCKS-2;i<NVJ_BLOCKS;i++) {
        int rc=transfer(&j,NVJ_START+i,b,1,"format-commit"); if(rc) return rc;
    }
    int rc=controls(&j,RETIRED); if(rc) return rc;
    memset(b,0,sizeof(b)); put64(b,ANCHOR); put32(b+8,VERSION); put32(b+12,(uint32_t)metadata_only);
    put64(b+16,bytes); memcpy(b+24,j.uuid,16); put32(b+40,NVJ_START); put32(b+44,NVJ_BLOCKS); seal(b);
    /* The stable anchor is published last, after initialized home metadata. */
    rc=barrier(&j,"format-flush");
    if(!rc) rc=transfer(&j,NVJ_START,b,1,"anchor-write");
    if(!rc) rc=barrier(&j,"anchor-flush");
    return rc;
}
/* Discover the volume bound without trusting mutable superblock bytes. */
int nvj_probe(const struct nvj_ops *ops,uint64_t device_bytes,uint64_t *volume_bytes,int metadata_only) {
    if(device_bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK) return -EINVAL;
    unsigned char a[NVJ_BLOCK];
    int rc=ops->transfer(ops->context,(uint64_t)NVJ_START*NVJ_BLOCK,NVJ_BLOCK,a,0);
    if(rc) return rc;
    if(get64(a)!=ANCHOR) return -ENOENT;
    uint64_t bytes=get64(a+16);
    if(!valid(a,ANCHOR) || get32(a+8)!=VERSION || get32(a+12)!=(unsigned)metadata_only ||
       get32(a+40)!=NVJ_START || get32(a+44)!=NVJ_BLOCKS || bytes>device_bytes ||
       bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK || bytes/NVJ_BLOCK>UINT32_MAX ||
       bytes%((uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK)) return -EINVAL;
    *volume_bytes=bytes; return 0;
}
int nvj_open(struct nvj **out,const struct nvj_ops *ops,uint64_t bytes,int metadata_only) {
    *out=NULL;
    if(bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK || bytes/NVJ_BLOCK>UINT32_MAX) return -EINVAL;
    struct nvj *j=calloc(1,sizeof(*j)); if(!j) return -ENOMEM;
    j->ops=*ops; j->bytes=bytes;
    unsigned char a[NVJ_BLOCK],c[2][NVJ_BLOCK],cert[2][NVJ_BLOCK];
    int rc=transfer(j,NVJ_START,a,0,"anchor-read");
    if(!rc && (!valid(a,ANCHOR) || get32(a+8)!=VERSION || get32(a+12)!=(unsigned)metadata_only ||
       get64(a+16)!=bytes || get32(a+40)!=NVJ_START || get32(a+44)!=NVJ_BLOCKS)) rc=-EINVAL;
    if(rc) { free(j); return rc; }
    memcpy(j->uuid,a+24,16);
    int pick=-1;
    for(unsigned i=0;i<2;i++) {
        rc=transfer(j,NVJ_START+1+i,c[i],0,"control-read"); if(rc) break;
        if(valid(c[i],CONTROL) && !memcmp(c[i]+16,j->uuid,16) && get32(c[i]+32)<=RETIRED) {
            if(pick<0 || get64(c[i]+8)>get64(c[pick]+8) ||
               (get64(c[i]+8)==get64(c[pick]+8) && get32(c[i]+32)>get32(c[pick]+32))) pick=(int)i;
        }
    }
    if(rc || pick<0) { free(j); return rc?rc:-EINVAL; }
    j->id=get64(c[pick]+8);
    if(get32(c[pick]+32)!=RETIRED) {
        int accepted=-1;
        for(unsigned i=0;i<2;i++) {
            rc=transfer(j,NVJ_START+NVJ_BLOCKS-2+i,cert[i],0,"commit-read"); if(rc) break;
            if(valid(cert[i],COMMIT) && same(j,cert[i])) {
                if(accepted>=0 && memcmp(cert[i],cert[accepted],NVJ_BLOCK)) { rc=-EINVAL; break; }
                accepted=(int)i;
            }
        }
        if(!rc && accepted>=0) rc=replay(j,cert[accepted]);
        else if(!rc) rc=controls(j,RETIRED);
        if(rc) { free(j); return rc; }
    }
    size_t blocks=(size_t)(bytes/NVJ_BLOCK);
    j->metadata=calloc((blocks+7)/8,1); j->index=calloc(blocks,sizeof(*j->index));
    j->allocated=NVJ_CAPACITY;
    j->entries=calloc(j->allocated,sizeof(*j->entries));
    j->images=malloc((size_t)j->allocated*NVJ_BLOCK);
    if(!j->metadata || !j->index || !j->images || !j->entries) { nvj_close(j); return -ENOMEM; }
    *out=j; return 0;
}
void nvj_close(struct nvj *j) { if(j) { free(j->metadata); free(j->index); free(j->images); free(j->entries); free(j); } }
void nvj_mark_metadata(struct nvj *j,uint32_t block) {
    if(j && block<j->bytes/NVJ_BLOCK) j->metadata[block/8]|=(unsigned char)(1u<<(block%8));
}
int nvj_begin(struct nvj *j,int full,unsigned budget) {
    if(!j || j->failed) return -EIO;
    if(j->active) return -EBUSY;
    if(!budget) budget=NVJ_CAPACITY;
    if(budget>NVJ_CAPACITY || (full!=0 && full!=1)) return -EINVAL;
    if(j->id==UINT64_MAX) return -EOVERFLOW;
    j->active=1; j->full=full; j->budget=budget; j->error=0;
    return 0;
}
void nvj_discard(struct nvj *j) {
    for(unsigned i=0;i<j->count;i++) j->index[j->entries[i].home]=0;
    j->count=0; j->active=0; j->error=0;
}
int nvj_transfer(struct nvj *j,uint64_t offset,size_t bytes,void *buf,int write_op) {
    if(write_op && j->failed) return -EIO;
    if(offset>j->bytes || bytes>j->bytes-offset) return -EINVAL;
    unsigned char *p=buf;
    while(bytes) {
        uint32_t block=(uint32_t)(offset/NVJ_BLOCK);
        unsigned within=(unsigned)(offset%NVJ_BLOCK);
        size_t n=NVJ_BLOCK-within; if(n>bytes) n=bytes;
        if(block>=NVJ_START && block<NVJ_START+NVJ_BLOCKS) return -EPERM;
        unsigned slot=j->index[block];
        int meta=!!(j->metadata[block/8] & (1u<<(block%8)));
        if(write_op && (meta || (j->active && j->full) || slot)) {
            if(!j->active) return -EPERM;
            if(!slot) {
                if(!within && n==NVJ_BLOCK) {
                    unsigned char old[NVJ_BLOCK];
                    int rc=transfer(j,block,old,0,"compare-read");
                    if(rc) return j->error=rc;
                    if(!memcmp(old,p,NVJ_BLOCK)) {
                        offset+=n; p+=n; bytes-=n; continue;
                    }
                }
                /* Keep the current legacy operation coherent even after its log
                 * budget is exhausted. These overflow images can only be
                 * discarded, never committed. The API boundary aborts the TX. */
                if(j->count==j->allocated) {
                    unsigned capacity=j->allocated*2;
                    if(capacity>j->bytes/NVJ_BLOCK) capacity=(unsigned)(j->bytes/NVJ_BLOCK);
                    struct entry *entries=realloc(j->entries,(size_t)capacity*sizeof(*entries));
                    if(!entries) return j->error=-ENOMEM;
                    j->entries=entries;
                    unsigned char *images=realloc(j->images,(size_t)capacity*NVJ_BLOCK);
                    if(!images) return j->error=-ENOMEM;
                    j->images=images; j->allocated=capacity;
                }
                if(j->count>=j->budget) j->error=-E2BIG;
                slot=++j->count; j->index[block]=slot;
                j->entries[slot-1]=(struct entry){.home=block,.kind=(uint32_t)meta};
                if(within || n!=NVJ_BLOCK) {
                    int rc=transfer(j,block,j->images+(size_t)(slot-1)*NVJ_BLOCK,0,"stage-read");
                    if(rc) return j->error=rc;
                }
                if(j->count>j->stats.peak_blocks) j->stats.peak_blocks=j->count;
            }
            memcpy(j->images+(size_t)(slot-1)*NVJ_BLOCK+within,p,n);
        } else if(!write_op && slot) memcpy(p,j->images+(size_t)(slot-1)*NVJ_BLOCK+within,n);
        else {
            int rc=j->ops.transfer(j->ops.context,offset,n,p,write_op);
            if(rc) { if(j->active) j->error=rc; return rc; }
        }
        offset+=n; p+=n; bytes-=n;
    }
    return 0;
}
int nvj_commit(struct nvj *j) {
    if(!j || !j->active) return -EINVAL;
    if(j->error) return j->error;
    if(!j->count) {
        int rc=barrier(j,"ordered-data-flush");
        if(rc) j->failed=1;
        else nvj_discard(j);
        return rc;
    }
    unsigned char *d=calloc(NVJ_DESCRIPTORS,NVJ_BLOCK);
    if(!d) return -ENOMEM;
    j->id++;
    identity(j,d,DESCRIPTOR); put32(d+32,j->count); put32(d+36,(uint32_t)j->full);
    for(unsigned i=0;i<j->count;i++) {
        unsigned char *e=d+HEADER+i*16;
        put32(e,j->entries[i].home); put32(e+4,j->entries[i].kind);
        put32(e+8,crc(j->images+(size_t)i*NVJ_BLOCK,NVJ_BLOCK));
    }
    int rc=barrier(j,"ordered-data-flush");
    if(!rc) rc=controls(j,PREPARED);
    for(unsigned i=0;i<NVJ_DESCRIPTORS && !rc;i++) rc=transfer(j,NVJ_START+3+i,d+i*NVJ_BLOCK,1,"descriptor-write");
    for(unsigned i=0;i<j->count && !rc;i++) rc=transfer(j,NVJ_START+NVJ_PAYLOAD+i,j->images+(size_t)i*NVJ_BLOCK,1,"payload-write");
    if(!rc) rc=barrier(j,"payload-flush");
    unsigned char cert[NVJ_BLOCK]; identity(j,cert,COMMIT);
    put32(cert+32,j->count); put32(cert+36,crc(d,NVJ_DESCRIPTORS*NVJ_BLOCK)); put32(cert+40,(uint32_t)j->full); seal(cert);
    for(unsigned i=0;i<2 && !rc;i++) {
        rc=transfer(j,NVJ_START+NVJ_BLOCKS-2+i,cert,1,"commit-write");
        if(!rc) rc=barrier(j,"commit-flush");
    }
    for(unsigned i=0;i<j->count && !rc;i++) rc=transfer(j,j->entries[i].home,j->images+(size_t)i*NVJ_BLOCK,1,"checkpoint-write");
    if(!rc) rc=barrier(j,"checkpoint-flush");
    if(!rc) rc=controls(j,RETIRED);
    if(!rc) {
        j->stats.commits++; j->stats.journal_bytes+=(uint64_t)(NVJ_DESCRIPTORS+j->count+6)*NVJ_BLOCK;
        j->stats.home_bytes+=(uint64_t)j->count*NVJ_BLOCK; nvj_discard(j);
    } else j->failed=1; /* No further writes after an uncertain commit. */
    free(d); return rc;
}
int nvj_flush(struct nvj *j) { return barrier(j,"data-flush"); }
int nvj_error(const struct nvj *j) { return j? (j->failed?-EIO:j->error):0; }
void nvj_fail(struct nvj *j,int error) { if(j && error<0 && !j->error) j->error=error; }
void nvj_require_recovery(struct nvj *j) { if(j) j->failed=1; }
int nvj_active(const struct nvj *j) { return j && j->active; }
int nvj_recovery_required(const struct nvj *j) { return j && j->failed; }
const struct nvj_stats *nvj_stats(const struct nvj *j) { return &j->stats; }
void nvj_set_hook(struct nvj *j,nvj_hook h,void *arg) { j->hook=h; j->hook_arg=arg; }
