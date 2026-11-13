/* Single-writer redo journal. The disk ABI uses explicit little-endian fields.
 * Home writes follow TWO independently flushed commit certificates. Controls
 * are retired in both copies before the next slot can overwrite payload. */
#include "nvfuse_journal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define NVJ_X86_CRC 1
#elif defined(__aarch64__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_acle.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#define NVJ_ARM_CRC 1
#elif defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#define NVJ_ARM_CRC 1
#endif
#endif

#define ANCHOR UINT64_C(0x314c4e524a46564e)
#define CONTROL UINT64_C(0x314c52544346564e)
#define COMMIT UINT64_C(0x31544d4f4346564e)
#define DESCRIPTOR UINT64_C(0x314353454446564e)
#define VERSION 2u
#define RETIRED 1u
#define PREPARED 0u
#define HEADER 128u
#define CHECKSUM (NVJ_BLOCK - 4u)
struct entry { uint32_t home, kind, checksum; };
struct bucket { uint32_t block, value; };
struct sparse { struct bucket *buckets; unsigned capacity, used; };
struct nvj_v3;
struct nvj {
    struct nvj_ops ops;
    uint64_t bytes, id;
    unsigned char uuid[16];
    unsigned char *images;
    struct sparse metadata, index;
    unsigned version, start, blocks, descriptors, payload, capacity;
    int filesystem_metadata, classification_error, data_dirty;
    struct entry *entries;
    unsigned allocated;
    unsigned count, budget;
    int active, full, error, failed;
    struct nvj_stats stats;
    int async_checkpoint, pending, checkpoint_error;
    int batch_io, profile, compare_reads, log_join, sparse_reset;
    nvj_home_writer home_writer;
    void *home_context;
    pthread_t checkpoint_thread;
    nvj_io_fault fault; void *fault_arg;
    nvj_hook hook;
    void *hook_arg;
    struct nvj_v3 *v3;
};
static int v3_format(struct nvj *);
static int v3_open(struct nvj *);
static void v3_close(struct nvj *);
static int v3_commit(struct nvj *);
static int v3_checkpoint(struct nvj *);
static const unsigned char *v3_image(struct nvj *, uint32_t);
static size_t v3_memory_bytes(const struct nvj *);
/* Open addressing with a <= 50% load. Values use zero as the empty marker;
 * block zero and the full uint32 block domain are valid keys. No tombstones
 * are needed: the overlay is cleared as a unit, classification is append-only. */
static int wait_checkpoint(struct nvj *j) {
    if(j && j->pending) {
        pthread_join(j->checkpoint_thread,NULL); j->pending=0;
        return j->checkpoint_error;
    }
    return 0;
}
static uint64_t profile_start(struct nvj *j) {
    if(!j->profile) return 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static void profile_end(struct nvj *j,unsigned stage,uint64_t start) {
    if(j->profile) j->stats.profile_ns[stage]+=profile_start(j)-start;
}
int nvj_set_batch_io(struct nvj *j,int enabled) {
    wait_checkpoint(j);
    if(!j || (enabled!=0 && enabled!=1)) return -EINVAL;
    if(j->active) return -EBUSY;
    j->batch_io=enabled; return 0;
}
int nvj_set_compare_reads(struct nvj *j,int enabled) {
    wait_checkpoint(j);
    if(!j || (enabled!=0 && enabled!=1)) return -EINVAL;
    if(j->active) return -EBUSY;
    j->compare_reads=enabled; return 0;
}
int nvj_set_log_join(struct nvj *j,int enabled) {
    wait_checkpoint(j);
    if(!j || (enabled!=0 && enabled!=1)) return -EINVAL;
    if(j->failed) return -EIO;
    if(j->active) return -EBUSY;
    j->log_join=enabled; return 0;
}
int nvj_set_home_writer(struct nvj *j,nvj_home_writer writer,void *context) {
    wait_checkpoint(j);
    if(!j || j->failed) return -EIO;
    if(j->active) return -EBUSY;
    j->home_writer=writer; j->home_context=context; return 0;
}
void nvj_set_profile(struct nvj *j,int enabled) {
    wait_checkpoint(j); if(j) j->profile=!!enabled;
}
static unsigned hash_block(uint32_t block) {
    block ^= block >> 16; block *= UINT32_C(0x7feb352d);
    block ^= block >> 15; block *= UINT32_C(0x846ca68b);
    return block ^ (block >> 16);
}
static struct bucket *lookup(struct sparse *s,uint32_t block) {
    unsigned i=hash_block(block)&(s->capacity-1);
    while(s->buckets[i].value && s->buckets[i].block!=block) i=(i+1)&(s->capacity-1);
    return &s->buckets[i];
}
static int reserve(struct sparse *s,unsigned capacity) {
    struct sparse next={.capacity=capacity,.used=s->used};
    next.buckets=calloc(capacity,sizeof(*next.buckets));
    if(!next.buckets) return -ENOMEM;
    for(unsigned i=0;i<s->capacity;i++) if(s->buckets[i].value)
        *lookup(&next,s->buckets[i].block)=s->buckets[i];
    free(s->buckets); *s=next; return 0;
}
static int remember(struct sparse *s,uint32_t block,unsigned value) {
    struct bucket *b=lookup(s,block);
    if(b->value) { b->value=value; return 0; }
    if(s->used>=s->capacity/2) {
        if(s->capacity>UINT32_MAX/2) return -ENOMEM;
        int rc=reserve(s,s->capacity*2); if(rc) return rc;
        b=lookup(s,block);
    }
    *b=(struct bucket){block,value}; s->used++; return 0;
}
static int geometry(struct nvj *j,unsigned version,unsigned start,unsigned blocks) {
    if((blocks!=NVJ_BLOCKS && blocks!=8192u && blocks!=16384u) ||
       start!=NVJ_GROUP_BLOCKS-blocks || (version==1 && blocks!=NVJ_BLOCKS)) return -EINVAL;
    j->start=start; j->blocks=blocks;
    j->descriptors=(unsigned)(((uint64_t)(blocks-5)*16+HEADER+NVJ_BLOCK+15)/(NVJ_BLOCK+16));
    j->payload=3+j->descriptors; j->capacity=blocks-j->payload-2; return 0;
}
/* The discovery anchor stays at its v1 address. All other slots enumerate the
 * reserved extent in order, skipping that anchor. This also reproduces v1. */
static uint32_t log_block(const struct nvj *j,unsigned slot) {
    if(!slot) return NVJ_START;
    uint32_t block=j->start+slot-1;
    return block>=NVJ_START?block+1:block;
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t get64(const unsigned char *p) { return get32(p) | (uint64_t)get32(p+4)<<32; }
static void put32(unsigned char *p, uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=(unsigned char)(n>>(8*i)); }
static void put64(unsigned char *p, uint64_t n) { put32(p,(uint32_t)n); put32(p+4,(uint32_t)(n>>32)); }
/* Reflected CRC32C, identical to the v1 bitwise encoding. Immutable lookup
 * data is safe even when independent engines are used by different threads. */
static const uint32_t crc32c_table[256]={
    UINT32_C(0x00000000),UINT32_C(0xf26b8303),UINT32_C(0xe13b70f7),UINT32_C(0x1350f3f4),
    UINT32_C(0xc79a971f),UINT32_C(0x35f1141c),UINT32_C(0x26a1e7e8),UINT32_C(0xd4ca64eb),
    UINT32_C(0x8ad958cf),UINT32_C(0x78b2dbcc),UINT32_C(0x6be22838),UINT32_C(0x9989ab3b),
    UINT32_C(0x4d43cfd0),UINT32_C(0xbf284cd3),UINT32_C(0xac78bf27),UINT32_C(0x5e133c24),
    UINT32_C(0x105ec76f),UINT32_C(0xe235446c),UINT32_C(0xf165b798),UINT32_C(0x030e349b),
    UINT32_C(0xd7c45070),UINT32_C(0x25afd373),UINT32_C(0x36ff2087),UINT32_C(0xc494a384),
    UINT32_C(0x9a879fa0),UINT32_C(0x68ec1ca3),UINT32_C(0x7bbcef57),UINT32_C(0x89d76c54),
    UINT32_C(0x5d1d08bf),UINT32_C(0xaf768bbc),UINT32_C(0xbc267848),UINT32_C(0x4e4dfb4b),
    UINT32_C(0x20bd8ede),UINT32_C(0xd2d60ddd),UINT32_C(0xc186fe29),UINT32_C(0x33ed7d2a),
    UINT32_C(0xe72719c1),UINT32_C(0x154c9ac2),UINT32_C(0x061c6936),UINT32_C(0xf477ea35),
    UINT32_C(0xaa64d611),UINT32_C(0x580f5512),UINT32_C(0x4b5fa6e6),UINT32_C(0xb93425e5),
    UINT32_C(0x6dfe410e),UINT32_C(0x9f95c20d),UINT32_C(0x8cc531f9),UINT32_C(0x7eaeb2fa),
    UINT32_C(0x30e349b1),UINT32_C(0xc288cab2),UINT32_C(0xd1d83946),UINT32_C(0x23b3ba45),
    UINT32_C(0xf779deae),UINT32_C(0x05125dad),UINT32_C(0x1642ae59),UINT32_C(0xe4292d5a),
    UINT32_C(0xba3a117e),UINT32_C(0x4851927d),UINT32_C(0x5b016189),UINT32_C(0xa96ae28a),
    UINT32_C(0x7da08661),UINT32_C(0x8fcb0562),UINT32_C(0x9c9bf696),UINT32_C(0x6ef07595),
    UINT32_C(0x417b1dbc),UINT32_C(0xb3109ebf),UINT32_C(0xa0406d4b),UINT32_C(0x522bee48),
    UINT32_C(0x86e18aa3),UINT32_C(0x748a09a0),UINT32_C(0x67dafa54),UINT32_C(0x95b17957),
    UINT32_C(0xcba24573),UINT32_C(0x39c9c670),UINT32_C(0x2a993584),UINT32_C(0xd8f2b687),
    UINT32_C(0x0c38d26c),UINT32_C(0xfe53516f),UINT32_C(0xed03a29b),UINT32_C(0x1f682198),
    UINT32_C(0x5125dad3),UINT32_C(0xa34e59d0),UINT32_C(0xb01eaa24),UINT32_C(0x42752927),
    UINT32_C(0x96bf4dcc),UINT32_C(0x64d4cecf),UINT32_C(0x77843d3b),UINT32_C(0x85efbe38),
    UINT32_C(0xdbfc821c),UINT32_C(0x2997011f),UINT32_C(0x3ac7f2eb),UINT32_C(0xc8ac71e8),
    UINT32_C(0x1c661503),UINT32_C(0xee0d9600),UINT32_C(0xfd5d65f4),UINT32_C(0x0f36e6f7),
    UINT32_C(0x61c69362),UINT32_C(0x93ad1061),UINT32_C(0x80fde395),UINT32_C(0x72966096),
    UINT32_C(0xa65c047d),UINT32_C(0x5437877e),UINT32_C(0x4767748a),UINT32_C(0xb50cf789),
    UINT32_C(0xeb1fcbad),UINT32_C(0x197448ae),UINT32_C(0x0a24bb5a),UINT32_C(0xf84f3859),
    UINT32_C(0x2c855cb2),UINT32_C(0xdeeedfb1),UINT32_C(0xcdbe2c45),UINT32_C(0x3fd5af46),
    UINT32_C(0x7198540d),UINT32_C(0x83f3d70e),UINT32_C(0x90a324fa),UINT32_C(0x62c8a7f9),
    UINT32_C(0xb602c312),UINT32_C(0x44694011),UINT32_C(0x5739b3e5),UINT32_C(0xa55230e6),
    UINT32_C(0xfb410cc2),UINT32_C(0x092a8fc1),UINT32_C(0x1a7a7c35),UINT32_C(0xe811ff36),
    UINT32_C(0x3cdb9bdd),UINT32_C(0xceb018de),UINT32_C(0xdde0eb2a),UINT32_C(0x2f8b6829),
    UINT32_C(0x82f63b78),UINT32_C(0x709db87b),UINT32_C(0x63cd4b8f),UINT32_C(0x91a6c88c),
    UINT32_C(0x456cac67),UINT32_C(0xb7072f64),UINT32_C(0xa457dc90),UINT32_C(0x563c5f93),
    UINT32_C(0x082f63b7),UINT32_C(0xfa44e0b4),UINT32_C(0xe9141340),UINT32_C(0x1b7f9043),
    UINT32_C(0xcfb5f4a8),UINT32_C(0x3dde77ab),UINT32_C(0x2e8e845f),UINT32_C(0xdce5075c),
    UINT32_C(0x92a8fc17),UINT32_C(0x60c37f14),UINT32_C(0x73938ce0),UINT32_C(0x81f80fe3),
    UINT32_C(0x55326b08),UINT32_C(0xa759e80b),UINT32_C(0xb4091bff),UINT32_C(0x466298fc),
    UINT32_C(0x1871a4d8),UINT32_C(0xea1a27db),UINT32_C(0xf94ad42f),UINT32_C(0x0b21572c),
    UINT32_C(0xdfeb33c7),UINT32_C(0x2d80b0c4),UINT32_C(0x3ed04330),UINT32_C(0xccbbc033),
    UINT32_C(0xa24bb5a6),UINT32_C(0x502036a5),UINT32_C(0x4370c551),UINT32_C(0xb11b4652),
    UINT32_C(0x65d122b9),UINT32_C(0x97baa1ba),UINT32_C(0x84ea524e),UINT32_C(0x7681d14d),
    UINT32_C(0x2892ed69),UINT32_C(0xdaf96e6a),UINT32_C(0xc9a99d9e),UINT32_C(0x3bc21e9d),
    UINT32_C(0xef087a76),UINT32_C(0x1d63f975),UINT32_C(0x0e330a81),UINT32_C(0xfc588982),
    UINT32_C(0xb21572c9),UINT32_C(0x407ef1ca),UINT32_C(0x532e023e),UINT32_C(0xa145813d),
    UINT32_C(0x758fe5d6),UINT32_C(0x87e466d5),UINT32_C(0x94b49521),UINT32_C(0x66df1622),
    UINT32_C(0x38cc2a06),UINT32_C(0xcaa7a905),UINT32_C(0xd9f75af1),UINT32_C(0x2b9cd9f2),
    UINT32_C(0xff56bd19),UINT32_C(0x0d3d3e1a),UINT32_C(0x1e6dcdee),UINT32_C(0xec064eed),
    UINT32_C(0xc38d26c4),UINT32_C(0x31e6a5c7),UINT32_C(0x22b65633),UINT32_C(0xd0ddd530),
    UINT32_C(0x0417b1db),UINT32_C(0xf67c32d8),UINT32_C(0xe52cc12c),UINT32_C(0x1747422f),
    UINT32_C(0x49547e0b),UINT32_C(0xbb3ffd08),UINT32_C(0xa86f0efc),UINT32_C(0x5a048dff),
    UINT32_C(0x8ecee914),UINT32_C(0x7ca56a17),UINT32_C(0x6ff599e3),UINT32_C(0x9d9e1ae0),
    UINT32_C(0xd3d3e1ab),UINT32_C(0x21b862a8),UINT32_C(0x32e8915c),UINT32_C(0xc083125f),
    UINT32_C(0x144976b4),UINT32_C(0xe622f5b7),UINT32_C(0xf5720643),UINT32_C(0x07198540),
    UINT32_C(0x590ab964),UINT32_C(0xab613a67),UINT32_C(0xb831c993),UINT32_C(0x4a5a4a90),
    UINT32_C(0x9e902e7b),UINT32_C(0x6cfbad78),UINT32_C(0x7fab5e8c),UINT32_C(0x8dc0dd8f),
    UINT32_C(0xe330a81a),UINT32_C(0x115b2b19),UINT32_C(0x020bd8ed),UINT32_C(0xf0605bee),
    UINT32_C(0x24aa3f05),UINT32_C(0xd6c1bc06),UINT32_C(0xc5914ff2),UINT32_C(0x37faccf1),
    UINT32_C(0x69e9f0d5),UINT32_C(0x9b8273d6),UINT32_C(0x88d28022),UINT32_C(0x7ab90321),
    UINT32_C(0xae7367ca),UINT32_C(0x5c18e4c9),UINT32_C(0x4f48173d),UINT32_C(0xbd23943e),
    UINT32_C(0xf36e6f75),UINT32_C(0x0105ec76),UINT32_C(0x12551f82),UINT32_C(0xe03e9c81),
    UINT32_C(0x34f4f86a),UINT32_C(0xc69f7b69),UINT32_C(0xd5cf889d),UINT32_C(0x27a40b9e),
    UINT32_C(0x79b737ba),UINT32_C(0x8bdcb4b9),UINT32_C(0x988c474d),UINT32_C(0x6ae7c44e),
    UINT32_C(0xbe2da0a5),UINT32_C(0x4c4623a6),UINT32_C(0x5f16d052),UINT32_C(0xad7d5351),
};
static uint32_t crc_software(const void *buf,size_t len) {
    const unsigned char *p=buf; uint32_t c=~0u;
    while(len--) c=(c>>8)^crc32c_table[(c^*p++)&255u];
    return ~c;
}
/* Same reflected Castagnoli polynomial, initial/final complement and byte
 * order as the software/disk ABI. memcpy handles unaligned and aliased input.
 * No ISA instruction executes until process-wide runtime feature detection. */
#ifdef NVJ_X86_CRC
__attribute__((target("sse4.2")))
static uint32_t crc_accelerated(const void *buf,size_t len) {
    const unsigned char *p=buf; uint64_t c=~0u;
    while(len>=8) {
        uint64_t word; memcpy(&word,p,sizeof(word));
        c=_mm_crc32_u64(c,word); p+=8; len-=8;
    }
    while(len--) c=_mm_crc32_u8((uint32_t)c,*p++);
    return ~(uint32_t)c;
}
static int crc_cpu_supported(void) {
    __builtin_cpu_init(); return !!__builtin_cpu_supports("sse4.2");
}
#define NVJ_CRC_NAME "x86-sse4.2"
#elif defined(NVJ_ARM_CRC)
#ifdef __clang__
__attribute__((target("crc")))
#else
__attribute__((target("+crc")))
#endif
static uint32_t crc_accelerated(const void *buf,size_t len) {
    const unsigned char *p=buf; uint32_t c=~0u;
    while(len>=8) {
        uint64_t word; memcpy(&word,p,sizeof(word));
        c=__crc32cd(c,word); p+=8; len-=8;
    }
    while(len--) c=__crc32cb(c,*p++);
    return ~c;
}
static int crc_cpu_supported(void) {
#ifdef __APPLE__
    int enabled=0; size_t n=sizeof(enabled);
    return !sysctlbyname("hw.optional.armv8_crc32",&enabled,&n,NULL,0) && enabled;
#else
    return !!(getauxval(AT_HWCAP)&HWCAP_CRC32);
#endif
}
#define NVJ_CRC_NAME "arm-crc32"
#endif
static pthread_once_t crc_once=PTHREAD_ONCE_INIT;
static uint32_t (*crc_impl)(const void *,size_t)=crc_software;
static const char *crc_name="software";
static int crc_policy_error;
static void crc_initialize(void) {
    const char *mode=getenv("NVFUSE_JOURNAL_CRC");
    if(mode && strcmp(mode,"auto") && strcmp(mode,"software")) {
        crc_policy_error=-EINVAL; crc_name="invalid-policy"; return;
    }
    if(mode && !strcmp(mode,"software")) return;
#if defined(NVJ_X86_CRC) || defined(NVJ_ARM_CRC)
    if(crc_cpu_supported()) { crc_impl=crc_accelerated; crc_name=NVJ_CRC_NAME; }
#endif
}
const char *nvj_crc_backend(void) {
    pthread_once(&crc_once,crc_initialize); return crc_name;
}
static int crc_policy(void) {
    pthread_once(&crc_once,crc_initialize); return crc_policy_error;
}
static uint32_t crc(const void *buf,size_t len) {
    pthread_once(&crc_once,crc_initialize); return crc_impl(buf,len);
}
static void seal(unsigned char *b) { put32(b+CHECKSUM,crc(b,CHECKSUM)); }
static int valid(const unsigned char *b,uint64_t magic) { return get64(b)==magic && get32(b+CHECKSUM)==crc(b,CHECKSUM); }
static void event(struct nvj *j,const char *name,uint64_t block) { if(j->hook) j->hook(j->hook_arg,name,block); }
static int physical(struct nvj *j,uint64_t offset,size_t bytes,void *buf,int write_op) {
    int rc=j->fault?j->fault(j->fault_arg,offset,bytes,write_op):0;
    return rc?rc:j->ops.transfer(j->ops.context,offset,bytes,buf,write_op);
}
static int transfer(struct nvj *j,uint32_t b,void *p,int write_op,const char *phase) {
    event(j,"before-io",b);
    int rc=physical(j,(uint64_t)b*NVJ_BLOCK,NVJ_BLOCK,p,write_op);
    if(!rc) event(j,phase,b);
    return rc;
}
/* Only contiguous addresses AND contiguous images may be coalesced. A batch
 * can tear or fail partway; existing certificates/flushes still govern replay.
 * Keep per-block hook/fault semantics for diagnostic users. */
static unsigned run_limit(struct nvj *j) {
    return j->batch_io && !j->hook && !j->fault ? 32u : 1u;
}
static int write_run(struct nvj *j,uint32_t block,unsigned count,void *buf,const char *phase) {
    if(count==1) return transfer(j,block,buf,1,phase);
    return physical(j,(uint64_t)block*NVJ_BLOCK,(size_t)count*NVJ_BLOCK,buf,1);
}
static int write_log(struct nvj *j,unsigned slot,unsigned count,unsigned char *buf,const char *phase) {
    int rc=0;
    for(unsigned i=0;i<count && !rc;) {
        uint32_t block=log_block(j,slot+i);
        unsigned n=1;
        while(n<run_limit(j) && i+n<count && log_block(j,slot+i+n)==block+n) n++;
        rc=write_run(j,block,n,buf+(size_t)i*NVJ_BLOCK,phase); i+=n;
    }
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
        int rc=transfer(j,log_block(j,i),b,1,"control-write"); if(rc) return rc;
        rc=barrier(j,state==RETIRED?"retire-flush":"prepare-flush"); if(rc) return rc;
    }
    return 0;
}
static int replay(struct nvj *j,const unsigned char *cert) {
    unsigned n=get32(cert+32);
    if(!n || n>j->capacity || get32(cert+40)>1) return -EINVAL;
    unsigned char *d=malloc((size_t)j->descriptors*NVJ_BLOCK);
    unsigned char *images=malloc((size_t)n*NVJ_BLOCK);
    if(!d || !images) { free(d); free(images); return -ENOMEM; }
    int rc=0;
    for(unsigned i=0;i<j->descriptors && !rc;i++) rc=transfer(j,log_block(j,3+i),d+i*NVJ_BLOCK,0,"descriptor-read");
    if(!rc && (get64(d)!=DESCRIPTOR || !same(j,d) || get32(d+32)!=n ||
       get32(d+36)!=get32(cert+40) || crc(d,(size_t)j->descriptors*NVJ_BLOCK)!=get32(cert+36))) rc=-EINVAL;
    /* Validate EVERY payload and destination before any replay home write. */
    for(unsigned i=0;i<n && !rc;i++) {
        unsigned char *e=d+HEADER+i*16;
        uint32_t home=get32(e),kind=get32(e+4);
        if(home>=j->bytes/NVJ_BLOCK || (home>=j->start && home<NVJ_GROUP_BLOCKS) ||
           kind>1 || (!get32(cert+40) && !kind)) { rc=-EINVAL; break; }
        for(unsigned k=0;k<i;k++) if(get32(d+HEADER+k*16)==home) rc=-EINVAL;
        if(rc) break;
        rc=transfer(j,log_block(j,j->payload+i),images+(size_t)i*NVJ_BLOCK,0,"payload-read");
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
int nvj_format_version(const struct nvj_ops *ops,uint64_t bytes,int metadata_only,unsigned blocks,unsigned version) {
    if(crc_policy()) return -EINVAL;
    if(version!=2 && version!=3) return -EINVAL;
    if(bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK || bytes/NVJ_BLOCK>UINT32_MAX ||
       bytes%((uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK) || (metadata_only!=0 && metadata_only!=1)) return -EINVAL;
    struct nvj j={.ops=*ops,.bytes=bytes};
    if(geometry(&j,VERSION,NVJ_GROUP_BLOCKS-blocks,blocks)) return -EINVAL;
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
    for(unsigned i=j.blocks-2;i<j.blocks;i++) {
        int rc=transfer(&j,log_block(&j,i),b,1,"format-commit"); if(rc) return rc;
    }
    int rc=version==3?v3_format(&j):controls(&j,RETIRED); if(rc) return rc;
    memset(b,0,sizeof(b)); put64(b,ANCHOR); put32(b+8,version); put32(b+12,(uint32_t)metadata_only);
    put64(b+16,bytes); memcpy(b+24,j.uuid,16); put32(b+40,j.start); put32(b+44,j.blocks); seal(b);
    /* The stable anchor is published last, after initialized home metadata. */
    rc=barrier(&j,"format-flush");
    if(!rc) rc=transfer(&j,NVJ_START,b,1,"anchor-write");
    if(!rc) rc=barrier(&j,"anchor-flush");
    return rc;
}
int nvj_format_sized(const struct nvj_ops *ops,uint64_t bytes,int metadata_only,unsigned blocks) {
    return nvj_format_version(ops,bytes,metadata_only,blocks,VERSION);
}
int nvj_format(const struct nvj_ops *ops,uint64_t bytes,int metadata_only) {
    return nvj_format_sized(ops,bytes,metadata_only,NVJ_BLOCKS);
}
/* Discover the volume bound without trusting mutable superblock bytes. */
int nvj_probe(const struct nvj_ops *ops,uint64_t device_bytes,uint64_t *volume_bytes,int metadata_only) {
    if(crc_policy()) return -EINVAL;
    if(device_bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK) return -EINVAL;
    unsigned char a[NVJ_BLOCK];
    int rc=ops->transfer(ops->context,(uint64_t)NVJ_START*NVJ_BLOCK,NVJ_BLOCK,a,0);
    if(rc) return rc;
    if(get64(a)!=ANCHOR) return -ENOENT;
    uint64_t bytes=get64(a+16);
    if(!valid(a,ANCHOR) || (get32(a+8)<1u || get32(a+8)>3u) || get32(a+12)!=(unsigned)metadata_only ||
       bytes>device_bytes ||
       bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK || bytes/NVJ_BLOCK>UINT32_MAX ||
       bytes%((uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK)) return -EINVAL;
    struct nvj check={0};
    if(geometry(&check,get32(a+8),get32(a+40),get32(a+44))) return -EINVAL;
    *volume_bytes=bytes; return 0;
}
int nvj_open(struct nvj **out,const struct nvj_ops *ops,uint64_t bytes,int metadata_only) {
    *out=NULL;
    if(crc_policy()) return -EINVAL;
    const char *reset=getenv("NVFUSE_JOURNAL_SPARSE_RESET");
    if(reset && strcmp(reset,"0") && strcmp(reset,"1")) return -EINVAL;
    if(bytes<(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK || bytes/NVJ_BLOCK>UINT32_MAX ||
       bytes%((uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK) || (metadata_only!=0 && metadata_only!=1)) return -EINVAL;
    struct nvj *j=calloc(1,sizeof(*j)); if(!j) return -ENOMEM;
    j->ops=*ops; j->bytes=bytes; j->compare_reads=1;
    j->sparse_reset=!reset || strcmp(reset,"0");
    unsigned char a[NVJ_BLOCK],c[2][NVJ_BLOCK],cert[2][NVJ_BLOCK];
    int rc=transfer(j,NVJ_START,a,0,"anchor-read");
    if(!rc && (!valid(a,ANCHOR) || (get32(a+8)<1u || get32(a+8)>3u) || get32(a+12)!=(unsigned)metadata_only ||
       get64(a+16)!=bytes)) rc=-EINVAL;
    if(!rc) rc=geometry(j,get32(a+8),get32(a+40),get32(a+44));
    if(rc) { free(j); return rc; }
    j->version=get32(a+8); memcpy(j->uuid,a+24,16);
    if(j->version==3) goto memory;
    int pick=-1;
    for(unsigned i=0;i<2;i++) {
        rc=transfer(j,log_block(j,1+i),c[i],0,"control-read"); if(rc) break;
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
            rc=transfer(j,log_block(j,j->blocks-2+i),cert[i],0,"commit-read"); if(rc) break;
            if(valid(cert[i],COMMIT) && same(j,cert[i])) {
                if(accepted>=0 && memcmp(cert[i],cert[accepted],NVJ_BLOCK)) { rc=-EINVAL; break; }
                accepted=(int)i;
            }
        }
        if(!rc && accepted>=0) rc=replay(j,cert[accepted]);
        else if(!rc) rc=controls(j,RETIRED);
        if(rc) { free(j); return rc; }
    }
memory: ;
    int memory_error=reserve(&j->metadata,64);
    unsigned index_capacity=16384;
    while(index_capacity/2<j->capacity) index_capacity*=2;
    if(!memory_error) memory_error=reserve(&j->index,index_capacity);
    j->allocated=j->capacity;
    j->entries=calloc(j->allocated,sizeof(*j->entries));
    j->images=malloc((size_t)j->allocated*NVJ_BLOCK);
    if(memory_error || !j->images || !j->entries) { nvj_close(j); return -ENOMEM; }
    if(j->version==3 && (rc=v3_open(j))) { nvj_close(j); return rc; }
    *out=j; return 0;
}
void nvj_close(struct nvj *j) { if(j) { wait_checkpoint(j); v3_close(j); free(j->metadata.buckets); free(j->index.buckets); free(j->images); free(j->entries); free(j); } }
void nvj_mark_filesystem_metadata(struct nvj *j) {
    if(j) j->filesystem_metadata=1;
}
void nvj_mark_metadata(struct nvj *j,uint32_t block) {
    if(!j || block>=j->bytes/NVJ_BLOCK || j->classification_error) return;
    if(j->filesystem_metadata && block%NVJ_GROUP_BLOCKS<4100) return;
    int rc=remember(&j->metadata,block,1);
    /* A failed classification must never let metadata bypass the journal.
     * Keep this sticky across discard; reopening reconstructs classification. */
    if(rc) j->classification_error=rc;
}
/* Called at an API boundary, before acquiring core cache/inode locks. */
int nvj_prepare_metadata(struct nvj *j,unsigned additional) {
    wait_checkpoint(j);
    if(!j || j->failed) return -EIO;
    if(j->classification_error) return j->classification_error;
    uint64_t needed=(uint64_t)j->metadata.used+additional;
    unsigned capacity=j->metadata.capacity;
    while(needed>capacity/2) {
        if(capacity>UINT32_MAX/2) return -ENOMEM;
        capacity*=2;
    }
    if(capacity!=j->metadata.capacity) { int rc=reserve(&j->metadata,capacity);if(rc) return rc; }
    /* Legacy core operations cannot unwind allocation while holding cache
     * locks. Reserve their bounded overflow workspace here, before mutation.
     * It can only be discarded, never committed beyond the journal budget. */
    if(j->active && j->count && (uint64_t)j->count+2048>j->index.capacity/2) {
        unsigned next=j->index.capacity;
        while((uint64_t)j->count+2048>next/2) next*=2;
        int rc=reserve(&j->index,next);if(rc) return rc;
    }
    if(j->active && j->count && j->allocated-j->count<2048u) {
        unsigned next=j->allocated*2;
        if(next>j->bytes/NVJ_BLOCK) next=(unsigned)(j->bytes/NVJ_BLOCK);
        struct entry *entries=realloc(j->entries,(size_t)next*sizeof(*entries));
        if(!entries) return -ENOMEM;
        j->entries=entries;
        unsigned char *images=realloc(j->images,(size_t)next*NVJ_BLOCK);
        if(!images) return -ENOMEM;
        j->images=images;j->allocated=next;
    }
    return 0;
}
unsigned nvj_budget(const struct nvj *j) { wait_checkpoint((struct nvj *)j); return j && j->active?j->budget:0; }
int nvj_full_data(const struct nvj *j) { wait_checkpoint((struct nvj *)j); return j && j->active && j->full; }
int nvj_begin(struct nvj *j,int full,unsigned budget) {
    wait_checkpoint(j);
    if(!j || j->failed) return -EIO;
    if(j->classification_error) return j->classification_error;
    if(j->active) return -EBUSY;
    if(!budget) budget=j->capacity;
    if(budget>j->capacity || (full!=0 && full!=1)) return -EINVAL;
    if(j->id==UINT64_MAX) return -EOVERFLOW;
    j->active=1; j->full=full; j->budget=budget; j->error=0;
    return 0;
}
static void discard(struct nvj *j) {
    if(j->count && j->sparse_reset && j->count<j->index.capacity/8) {
        /* Discard the entire set, with no lookup or insertion interleaved.
         * Earlier removals leave holes in probe chains: deliberately search
         * THROUGH those holes until the remaining known key is found. A
         * normal lookup would stop early and leave stale transaction images.
         * Dense transactions retain the linear memset path. */
        unsigned mask=j->index.capacity-1;
        for(unsigned n=0;n<j->count;n++) {
            unsigned slot=hash_block(j->entries[n].home)&mask,probes=0;
            while((!j->index.buckets[slot].value ||
                   j->index.buckets[slot].block!=j->entries[n].home) &&
                  probes<j->index.capacity) { slot=(slot+1)&mask; probes++; }
            if(probes==j->index.capacity) {
                /* Also clear safely if an internal invariant is violated. */
                memset(j->index.buckets,0,(size_t)j->index.capacity*sizeof(*j->index.buckets));
                break;
            }
            j->index.buckets[slot]=(struct bucket){0};
        }
    } else if(j->count) memset(j->index.buckets,0,(size_t)j->index.capacity*sizeof(*j->index.buckets));
    j->index.used=0; j->data_dirty=0;
    j->count=0; j->active=0; j->error=0;
}
void nvj_discard(struct nvj *j) { wait_checkpoint(j); discard(j); }
int nvj_needs_overlay(struct nvj *j,uint64_t offset,size_t bytes,int write_op) {
    wait_checkpoint(j);
    if(!j || j->failed || j->classification_error || offset>j->bytes || bytes>j->bytes-offset) return 1;
    /* Most steady reads have no staged images. Skip per-block hashing while
     * still rejecting any overlap with the reserved journal extent. */
    if(!write_op && !j->index.used && !j->v3)
        return bytes && offset < (uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK &&
               offset+bytes > (uint64_t)j->start*NVJ_BLOCK;
    while(bytes) {
        uint32_t block=(uint32_t)(offset/NVJ_BLOCK);
        size_t n=NVJ_BLOCK-offset%NVJ_BLOCK; if(n>bytes) n=bytes;
        if((block>=j->start && block<NVJ_GROUP_BLOCKS) || (j->index.used && lookup(&j->index,block)->value) ||
           (j->v3 && v3_image(j,block)) ||
           (write_op && (j->full || (j->filesystem_metadata && block%NVJ_GROUP_BLOCKS<4100) ||
                         lookup(&j->metadata,block)->value))) return 1;
        offset+=n; bytes-=n;
    }
    return 0;
}
void nvj_note_data_write(struct nvj *j) { if(j) j->data_dirty=1; }
int nvj_transfer(struct nvj *j,uint64_t offset,size_t bytes,void *buf,int write_op) {
    wait_checkpoint(j);
    if(write_op && j->failed) return -EIO;
    if(j->classification_error) return j->classification_error;
    if(offset>j->bytes || bytes>j->bytes-offset) return -EINVAL;
    unsigned char *p=buf;
    while(bytes) {
        uint32_t block=(uint32_t)(offset/NVJ_BLOCK);
        unsigned within=(unsigned)(offset%NVJ_BLOCK);
        size_t n=NVJ_BLOCK-within; if(n>bytes) n=bytes;
        if(block>=j->start && block<NVJ_GROUP_BLOCKS) return -EPERM;
        unsigned slot=lookup(&j->index,block)->value;
        const unsigned char *committed=j->v3?v3_image(j,block):NULL;
        int meta=(j->filesystem_metadata && block%NVJ_GROUP_BLOCKS<4100) ||
                 lookup(&j->metadata,block)->value;
        if(write_op && (meta || (j->active && j->full) || slot || committed)) {
            if(!j->active) return -EPERM;
            if(!slot) {
                if(j->compare_reads && !committed && !within && n==NVJ_BLOCK) {
                    unsigned char old[NVJ_BLOCK];
                    uint64_t start=profile_start(j);
                    int rc=transfer(j,block,old,0,"compare-read");
                    int equal=!rc && !memcmp(old,p,NVJ_BLOCK);
                    if(j->profile) {
                        j->stats.compare_reads++;
                        j->stats.compare_equal+=(unsigned)equal;
                        j->stats.compare_ns+=profile_start(j)-start;
                    }
                    if(rc) return j->error=rc;
                    if(equal) {
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
                slot=j->count+1;
                int added=remember(&j->index,block,slot);
                if(added) return j->error=added;
                j->count++;
                j->entries[slot-1]=(struct entry){.home=block,.kind=(uint32_t)meta};
                if(within || n!=NVJ_BLOCK) {
                    uint64_t start=profile_start(j);
                    int rc=0;
                    if(committed) memcpy(j->images+(size_t)(slot-1)*NVJ_BLOCK,committed,NVJ_BLOCK);
                    else rc=transfer(j,block,j->images+(size_t)(slot-1)*NVJ_BLOCK,0,"stage-read");
                    if(j->profile) j->stats.stage_read_ns+=profile_start(j)-start;
                    if(rc) return j->error=rc;
                }
                if(j->count>j->stats.peak_blocks) j->stats.peak_blocks=j->count;
            }
            memcpy(j->images+(size_t)(slot-1)*NVJ_BLOCK+within,p,n);
        } else if(!write_op && slot) memcpy(p,j->images+(size_t)(slot-1)*NVJ_BLOCK+within,n);
        else if(!write_op && committed) memcpy(p,committed+within,n);
        else {
            int rc=physical(j,offset,n,p,write_op);
            if(rc) { if(j->active) j->error=rc; return rc; }
            if(write_op) { j->data_dirty=1; j->stats.direct_bytes+=n; }
        }
        offset+=n; p+=n; bytes-=n;
    }
    return 0;
}
static int checkpoint(struct nvj *j) {
    int rc=0;
    uint64_t start=profile_start(j);
    for(unsigned i=0;i<j->count && !rc;) {
        if(j->home_writer && j->batch_io && !j->hook && !j->fault) {
            struct nvj_home_write writes[NVJ_HOME_BATCH_BLOCKS];
            unsigned end=i+NVJ_HOME_BATCH_BLOCKS, count=0;
            if(end>j->count) end=j->count;
            while(i<end) {
                unsigned n=1;
                while(i+n<end && j->entries[i+n].home==j->entries[i].home+n) n++;
                writes[count++]=(struct nvj_home_write){
                    (uint64_t)j->entries[i].home*NVJ_BLOCK,
                    (size_t)n*NVJ_BLOCK, j->images+(size_t)i*NVJ_BLOCK};
                i+=n;
            }
            rc=j->home_writer(j->home_context,writes,count);
            j->stats.home_batches++; j->stats.home_requests+=count;
            continue;
        }
        unsigned n=1;
        while(n<run_limit(j) && i+n<j->count && j->entries[i+n].home==j->entries[i].home+n) n++;
        rc=write_run(j,j->entries[i].home,n,j->images+(size_t)i*NVJ_BLOCK,"checkpoint-write"); i+=n;
    }
    profile_end(j,NVJ_P_HOME_WRITE,start); start=profile_start(j);
    if(!rc) rc=barrier(j,"checkpoint-flush");
    profile_end(j,NVJ_P_HOME_FLUSH,start); start=profile_start(j);
    if(!rc) rc=controls(j,RETIRED);
    profile_end(j,NVJ_P_RETIRE,start);
    if(!rc) {
        j->stats.commits++; j->stats.journal_bytes+=(uint64_t)(j->descriptors+j->count+6)*NVJ_BLOCK;
        j->stats.home_bytes+=(uint64_t)j->count*NVJ_BLOCK; discard(j);
    } else j->failed=1;
    return rc;
}
static void *checkpoint_worker(void *arg) {
    struct nvj *j=arg; j->checkpoint_error=checkpoint(j); return NULL;
}
int nvj_set_async_checkpoint(struct nvj *j,int enabled) {
    wait_checkpoint(j);
    if(!j || j->failed) return -EIO;
    if(enabled!=0 && enabled!=1) return -EINVAL;
    if(j->version==3 && enabled) return -ENOTSUP;
    if(j->active) return -EBUSY;
    j->async_checkpoint=enabled; return 0;
}
int nvj_commit(struct nvj *j) {
    wait_checkpoint(j);
    if(!j || !j->active) return -EINVAL;
    if(j->classification_error) return j->classification_error;
    if(j->error) return j->error;
    if(j->version==3) return v3_commit(j);
    if(!j->count) {
        int rc=j->data_dirty?barrier(j,"ordered-data-flush"):0;
        if(rc) j->failed=1;
        else discard(j);
        return rc;
    }
    uint64_t start=profile_start(j);
    /* Joining the boundary saves one owner/reactor round trip only when the
     * final payload run fits alongside the descriptors. Keep the existing
     * independent-write path otherwise, avoiding copying with no I/O gain.
     * No certificate is published until the same payload flush completes. */
    unsigned limit=run_limit(j), prefix=0;
    if(j->log_join && j->descriptors<limit && j->count%limit &&
       j->count%limit<=limit-j->descriptors) {
        prefix=limit-j->descriptors;
        if(prefix>j->count) prefix=j->count;
    }
    unsigned char *d=calloc(j->descriptors+prefix,NVJ_BLOCK);
    if(!d) return -ENOMEM;
    uint64_t allocated_at=profile_start(j);
    for(unsigned i=0;i<j->count;i++)
        j->entries[i].checksum=crc(j->images+(size_t)i*NVJ_BLOCK,NVJ_BLOCK);
    uint64_t crc_at=profile_start(j);
    j->id++;
    identity(j,d,DESCRIPTOR); put32(d+32,j->count); put32(d+36,(uint32_t)j->full);
    for(unsigned i=0;i<j->count;i++) {
        unsigned char *e=d+HEADER+i*16;
        put32(e,j->entries[i].home); put32(e+4,j->entries[i].kind);
        put32(e+8,j->entries[i].checksum);
    }
    if(prefix) memcpy(d+(size_t)j->descriptors*NVJ_BLOCK,j->images,(size_t)prefix*NVJ_BLOCK);
    uint64_t encoded_at=profile_start(j);
    if(j->profile) {
        j->stats.descriptor_alloc_ns+=allocated_at-start;
        j->stats.payload_crc_ns+=crc_at-allocated_at;
        j->stats.descriptor_encode_ns+=encoded_at-crc_at;
        j->stats.profile_ns[NVJ_P_DESCRIPTOR]+=encoded_at-start;
    }
    start=encoded_at;
    int rc=barrier(j,"ordered-data-flush");
    profile_end(j,NVJ_P_ORDERED,start); start=profile_start(j);
    if(!rc) rc=controls(j,PREPARED);
    profile_end(j,NVJ_P_PREPARE,start); start=profile_start(j);
    if(!rc) {
        if(prefix) { j->stats.log_join_commits++; j->stats.log_join_blocks+=prefix; }
        rc=write_log(j,3,j->descriptors+prefix,d,"descriptor-write");
    }
    if(!rc) rc=write_log(j,j->payload+prefix,j->count-prefix,
                         j->images+(size_t)prefix*NVJ_BLOCK,"payload-write");
    profile_end(j,NVJ_P_LOG_WRITE,start); start=profile_start(j);
    if(!rc) rc=barrier(j,"payload-flush");
    profile_end(j,NVJ_P_PAYLOAD_FLUSH,start); start=profile_start(j);
    uint64_t cert_start=profile_start(j);
    unsigned char cert[NVJ_BLOCK]; identity(j,cert,COMMIT);
    put32(cert+32,j->count); put32(cert+36,crc(d,(size_t)j->descriptors*NVJ_BLOCK)); put32(cert+40,(uint32_t)j->full); seal(cert);
    if(j->profile) j->stats.certificate_crc_ns+=profile_start(j)-cert_start;
    for(unsigned i=0;i<2 && !rc;i++) {
        rc=transfer(j,log_block(j,j->blocks-2+i),cert,1,"commit-write");
        if(!rc) rc=barrier(j,"commit-flush");
    }
    profile_end(j,NVJ_P_CERTIFICATE,start);
    free(d);
    if(rc) { j->failed=1; return rc; }
    if(j->async_checkpoint) {
        j->checkpoint_error=0;
        if(!pthread_create(&j->checkpoint_thread,NULL,checkpoint_worker,j)) { j->pending=1; return 0; }
    }
    return checkpoint(j);
}

int nvj_flush(struct nvj *j) {
    wait_checkpoint(j);
    if(j->failed) return -EIO;
    int rc=barrier(j,"data-flush");
    if(!rc) j->data_dirty=0;
    else if(j->active) j->error=rc;
    return rc;
}
int nvj_checkpoint(struct nvj *j) {
    wait_checkpoint(j);
    if(!j || j->failed) return -EIO;
    if(j->active) return -EBUSY;
    return j->v3?v3_checkpoint(j):0;
}
int nvj_error(const struct nvj *j) { wait_checkpoint((struct nvj *)j); return j? (j->failed?-EIO:(j->classification_error?j->classification_error:j->error)):0; }
void nvj_fail(struct nvj *j,int error) { wait_checkpoint(j); if(j && error<0 && !j->error) j->error=error; }
void nvj_require_recovery(struct nvj *j) { wait_checkpoint(j); if(j) j->failed=1; }
int nvj_active(const struct nvj *j) { wait_checkpoint((struct nvj *)j); return j && j->active; }
int nvj_recovery_required(const struct nvj *j) { wait_checkpoint((struct nvj *)j); return j && j->failed; }
const struct nvj_stats *nvj_stats(const struct nvj *j) { wait_checkpoint((struct nvj *)j); return &j->stats; }
size_t nvj_memory_bytes(const struct nvj *j) {
    return sizeof(*j)+(size_t)(j->metadata.capacity+j->index.capacity)*sizeof(struct bucket)+
        (size_t)j->allocated*(NVJ_BLOCK+sizeof(struct entry))+v3_memory_bytes(j);
}
void nvj_set_hook(struct nvj *j,nvj_hook h,void *arg) { wait_checkpoint(j); j->hook=h; j->hook_arg=arg; }

unsigned nvj_version(const struct nvj *j) { return j?j->version:0; }

unsigned nvj_capacity(const struct nvj *j) { return j?j->capacity:0; }
unsigned nvj_journal_start(const struct nvj *j) { return j?j->start:0; }

void nvj_set_io_fault_hook(struct nvj *j,nvj_io_fault hook,void *arg) {
    wait_checkpoint(j);j->fault=hook;j->fault_arg=arg;
}

#include "nvfuse_journal_v3.inc"
