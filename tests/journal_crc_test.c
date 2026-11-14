/* SPDX-License-Identifier: GPL-2.0-only */

/* Independent CRC oracle, including unaligned/tail inputs and process policy.
 * Include the engine to compare private implementations without exporting them. */
#include "../src/nvfuse_journal.c"
#include <assert.h>
#include <stdio.h>
static uint32_t bitwise(const void *buf,size_t len) {
    const unsigned char *p=buf; uint32_t c=~0u;
    while(len--) {
        c^=*p++;
        for(unsigned i=0;i<8;i++) c=(c>>1)^(UINT32_C(0x82f63b78)&(0u-(c&1u)));
    }
    return ~c;
}
static unsigned cases;
static void check(const unsigned char *p,size_t n) {
    uint32_t expected=bitwise(p,n);
    assert(crc_software(p,n)==expected);
    assert(crc(p,n)==expected);
#if defined(NVJ_X86_CRC) || defined(NVJ_ARM_CRC)
    if(crc_cpu_supported()) assert(crc_accelerated(p,n)==expected);
#endif
    cases++;
}
int main(void) {
    if(crc_policy()) {
        assert(!strcmp(nvj_crc_backend(),"invalid-policy"));
        assert(nvj_format_sized(NULL,(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK,0,NVJ_BLOCKS)==-EINVAL);
        uint64_t size=0; struct nvj *j=(void *)1;
        assert(nvj_probe(NULL,(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK,&size,0)==-EINVAL);
        assert(nvj_open(&j,NULL,(uint64_t)NVJ_GROUP_BLOCKS*NVJ_BLOCK,0)==-EINVAL && !j);
        puts("PASS invalid CRC policy rejected before disk callbacks"); return 0;
    }
    assert(crc("",0)==0);
    assert(crc("123456789",9)==UINT32_C(0xe3069283));
    const size_t sizes[]={0,1,7,8,9,15,16,17,31,32,63,64,65,127,128,255,256,4092,4096,65536,262144};
    for(unsigned pattern=0;pattern<4;pattern++) for(unsigned align=0;align<16;align++) {
        for(unsigned k=0;k<sizeof(sizes)/sizeof(sizes[0]);k++) {
            size_t n=sizes[k]; unsigned char *memory=malloc(n+align+1); assert(memory);
            unsigned char *p=memory+align; uint32_t state=0x8af13b57;
            for(size_t i=0;i<n;i++) {
                state^=state<<13; state^=state>>17; state^=state<<5;
                p[i]=pattern==0?0:pattern==1?255:pattern==2?(unsigned char)i:(unsigned char)state;
            }
            check(p,n);
            if(n) { p[n/2]^=0x81; check(p,n); }
            free(memory);
        }
    }
    printf("PASS CRC32C backend=%s: %u independent bitwise/software/ISA comparisons, known vectors, tails and alignments\n",nvj_crc_backend(),cases);
    return 0;
}
