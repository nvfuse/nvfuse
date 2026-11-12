/* Read-only, independent checker for the native file-image test format.
 * It never mounts, repairs, or calls the filesystem block-mapping routines. */
#include "nvfuse_core.h"
#include "nvfuse_journal.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"CHECK FAILED %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
struct node { uint32_t ino,type,bp,links,parent,actual_parent,dot_parent,blocks[14]; uint64_t size; };
static int image;
static unsigned groups,blocks,inodes;
static unsigned char *allocated,*live;
static uint32_t *owners,*references;
static struct node *nodes;
static uint64_t mapped,indirects,regular_bytes;
static int metadata_only;
static uint64_t semantic_hash=UINT64_C(14695981039346656037);
static void hash_bytes(const void *data,size_t count) {
    const unsigned char *p=data;
    while(count--) { semantic_hash^=*p++;semantic_hash*=UINT64_C(1099511628211); }
}
static void read_at(void *p,size_t n,uint64_t offset) { REQUIRE(pread(image,p,n,(off_t)offset)==(ssize_t)n); }
static void claim(unsigned block,unsigned ino) {
    REQUIRE(block<blocks && allocated[block] && !owners[block]); owners[block]=ino+1;
}
static void walk(unsigned block,unsigned level,unsigned ino,unsigned char *dir,uint64_t *logical,uint64_t *leaves) {
    if (!block) { *logical+=level==0?1:level==1?1024:level==2?1048576:1073741824; return; }
    claim(block,ino);
    if (level) {
        uint32_t pointers[1024]; read_at(pointers,sizeof(pointers),(uint64_t)block*NVJ_BLOCK); indirects++;
        for(unsigned k=0;k<1024;k++) walk(pointers[k],level-1,ino,dir,logical,leaves);
    } else {
        mapped++; (*leaves)++;
        if(dir) {
            REQUIRE(*logical<nodes[ino].size/NVJ_BLOCK);
            read_at(dir+*logical*NVJ_BLOCK,NVJ_BLOCK,(uint64_t)block*NVJ_BLOCK);
        } else if(metadata_only && nodes[ino].type==NVFUSE_TYPE_FILE) {
            unsigned char data[NVJ_BLOCK]; read_at(data,sizeof(data),(uint64_t)block*NVJ_BLOCK);
            for(unsigned k=0;k<sizeof(data);k++) REQUIRE(data[k]==0);
            regular_bytes+=sizeof(data);
        }
        (*logical)++;
    }
}
static int names(const void *a,const void *b) { return strcmp(*(const char *const *)a,*(const char *const *)b); }
int main(int argc,char **argv) {
    REQUIRE(argc==2 || (argc==3 && !strcmp(argv[2],"--skip-payload")));
    image=open(argv[1],O_RDONLY); REQUIRE(image>=0);
    struct nvfuse_superblock_common sb; read_at(&sb,sizeof(sb),0);
    REQUIRE((sb.sb_signature==NVJ_META_SIGNATURE || sb.sb_signature==NVJ_SIGNATURE) && sb.sb_state==FS_STATE_UMOUNTED);
    metadata_only=sb.sb_signature==NVJ_META_SIGNATURE && argc==2;
    groups=(unsigned)sb.sb_bg_num; blocks=(unsigned)sb.sb_no_of_blocks;
    REQUIRE(groups && blocks==groups*NVJ_GROUP_BLOCKS);
    inodes=groups*NVFUSE_INODE_PER_BG;
    allocated=calloc(blocks,1); live=calloc(inodes,1); owners=calloc(blocks,sizeof(*owners));
    references=calloc(inodes,sizeof(*references)); nodes=calloc(inodes,sizeof(*nodes));
    REQUIRE(allocated && live && owners && references && nodes);
    uint64_t free_blocks=0,free_inodes=0;
    for(unsigned bg=0;bg<groups;bg++) {
        struct nvfuse_bg_descriptor bd; unsigned char db[NVJ_BLOCK],ib[NVJ_BLOCK];
        read_at(&bd,sizeof(bd),((uint64_t)bg*NVJ_GROUP_BLOCKS+1)*NVJ_BLOCK);
        REQUIRE(bd.bd_magic==NVFUSE_BD_MAGIC && bd.bd_id==bg);
        REQUIRE(bd.bd_max_blocks==NVJ_GROUP_BLOCKS && bd.bd_max_inodes==NVFUSE_INODE_PER_BG);
        read_at(db,sizeof(db),(uint64_t)bd.bd_dbitmap_start*NVJ_BLOCK);
        read_at(ib,sizeof(ib),(uint64_t)bd.bd_ibitmap_start*NVJ_BLOCK);
        unsigned fb=0,fi=0;
        for(unsigned k=0;k<bd.bd_max_blocks;k++) {
            unsigned p=bg*NVJ_GROUP_BLOCKS+k; allocated[p]=!!(db[k/8]&(1u<<(k%8))); fb+=!allocated[p];
            if(k<4100 || (!bg && k>=NVJ_START)) { REQUIRE(allocated[p]); owners[p]=UINT32_MAX; }
        }
        for(unsigned k=0;k<bd.bd_max_inodes;k++) {
            unsigned ino=bg*NVFUSE_INODE_PER_BG+k; live[ino]=!!(ib[k/8]&(1u<<(k%8))); fi+=!live[ino];
            if(!live[ino] || (ino<NUM_RESV_INO && ino!=ROOT_INO)) continue;
            struct nvfuse_inode raw;
            read_at(&raw,sizeof(raw),(uint64_t)(bd.bd_itable_start+k)*NVJ_BLOCK);
            REQUIRE(raw.i_ino==ino && !raw.i_deleted);
            nodes[ino]=(struct node){.ino=ino,.type=raw.i_type,.bp=raw.i_bpino,.links=raw.i_links_count,.size=raw.i_size,.parent=raw.i_parent};
            memcpy(nodes[ino].blocks,raw.i_blocks,sizeof(nodes[ino].blocks));
        }
        REQUIRE(fb==bd.bd_free_blocks && fi==bd.bd_free_inodes); free_blocks+=fb; free_inodes+=fi;
    }
    REQUIRE(free_blocks==(uint64_t)sb.sb_free_blocks && free_inodes==(uint64_t)sb.sb_free_inodes);
    references[ROOT_INO]=1;nodes[ROOT_INO].actual_parent=ROOT_INO;
    for(unsigned ino=0;ino<inodes;ino++) {
        struct node *n=nodes+ino;
        if(!n->ino) continue;
        hash_bytes(&n->ino,sizeof(n->ino));hash_bytes(&n->type,sizeof(n->type));
        hash_bytes(&n->bp,sizeof(n->bp));hash_bytes(&n->links,sizeof(n->links));
        hash_bytes(&n->parent,sizeof(n->parent));hash_bytes(&n->size,sizeof(n->size));hash_bytes(n->blocks,sizeof(n->blocks));
        unsigned char *dir=NULL;
        if(n->type==NVFUSE_TYPE_DIRECTORY && n->size) {
            REQUIRE(n->size%NVJ_BLOCK==0 && n->size<=(uint64_t)blocks*NVJ_BLOCK);
            dir=calloc(1,(size_t)n->size); REQUIRE(dir);
        }
        uint64_t logical=0,leaves=0;
        for(unsigned k=0;k<DIRECT_BLOCKS;k++) walk(n->blocks[k],0,ino,dir,&logical,&leaves);
        for(unsigned k=INDIRECT_BLOCKS;k<=TINDIRECT_BLOCKS;k++) walk(n->blocks[k],k-INDIRECT_BLOCKS+1,ino,dir,&logical,&leaves);
        if(n->type==NVFUSE_TYPE_FILE || dir) REQUIRE(leaves==(n->size+NVJ_BLOCK-1)/NVJ_BLOCK);
        if(n->bp) { REQUIRE(n->bp<inodes && live[n->bp] && nodes[n->bp].type==NVFUSE_TYPE_BPTREE); references[n->bp]++; }
        if(dir) {
            unsigned entries=(unsigned)(n->size/DIR_ENTRY_SIZE),used=0; char **sorted=calloc(entries,sizeof(*sorted)); REQUIRE(sorted);
            struct nvfuse_dir_entry *d=(void *)dir;
            for(unsigned k=0;k<entries;k++) if(d[k].d_flag==DIR_USED) {
                REQUIRE(d[k].d_ino<inodes && live[d[k].d_ino]);
                REQUIRE(memchr(d[k].d_filename,0,FNAME_SIZE)); sorted[used++]=d[k].d_filename;
                hash_bytes(&d[k].d_ino,sizeof(d[k].d_ino));
                hash_bytes(d[k].d_filename,strlen(d[k].d_filename)+1);
                if(!strcmp(d[k].d_filename,".")) REQUIRE(d[k].d_ino==ino);
                else if(!strcmp(d[k].d_filename,"..")) n->dot_parent=d[k].d_ino;
                else {
                    references[d[k].d_ino]++;
                    if(nodes[d[k].d_ino].type==NVFUSE_TYPE_DIRECTORY) {
                        REQUIRE(!nodes[d[k].d_ino].actual_parent);
                        nodes[d[k].d_ino].actual_parent=ino;
                    }
                }
            }
            REQUIRE(used==n->links); qsort(sorted,used,sizeof(*sorted),names);
            for(unsigned k=1;k<used;k++) REQUIRE(strcmp(sorted[k-1],sorted[k]));
            free(sorted); free(dir);
        }
    }
    for(unsigned ino=0;ino<inodes;ino++) if(nodes[ino].ino && nodes[ino].type==NVFUSE_TYPE_DIRECTORY) {
        REQUIRE(nodes[ino].actual_parent);
        /* Zero is a legacy directory pending namespace-based parent backfill. */
        if(nodes[ino].parent) {
            REQUIRE(nodes[ino].parent==nodes[ino].actual_parent);
            if(nodes[ino].size) REQUIRE(nodes[ino].dot_parent==nodes[ino].parent);
        }
    }
    for(unsigned p=0;p<blocks;p++) REQUIRE(!allocated[p] || owners[p]);
    for(unsigned ino=NUM_RESV_INO;ino<inodes;ino++) if(live[ino]) REQUIRE(references[ino]>0);
    hash_bytes(allocated,blocks);hash_bytes(live,inodes);
    printf("CHECK {\"groups\":%u,\"mapped_blocks\":%" PRIu64 ",\"indirect_blocks\":%" PRIu64 ",\"regular_bytes_verified_zero\":%" PRIu64 ",\"free_blocks\":%" PRIu64 ",\"free_inodes\":%" PRIu64 ",\"semantic_hash\":\"%016" PRIx64 "\"}\n",groups,mapped,indirects,regular_bytes,free_blocks,free_inodes,semantic_hash);
    REQUIRE(!close(image)); free(allocated);free(live);free(owners);free(references);free(nodes); return 0;
}
