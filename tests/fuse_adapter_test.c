/* Actual FUSE 3 callbacks and journaled images, without a system mount. */
#define main nvfuse_fuse_main
#include "../examples/fuse3/nvfuse_fuse.c"
#undef main
#include "nvfuse_tx.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

struct listing { off_t next; unsigned count, total; };
static int fill(void *buf, const char *name, const struct stat *st, off_t next, enum fuse_fill_dir_flags flags)
{
    (void)name; (void)flags;
    struct listing *l = buf;
    if (l->count == 3) return 1;
    CHECK(st->st_ino && next > l->next);
    l->next = next; l->count++; l->total++;
    return 0;
}
static void verify(void)
{
    struct stat st;
    CHECK(!nf_getattr("/file", &st, NULL) && st.st_size == 8195 && (st.st_mode & 0777) == 0640);
    CHECK(st.st_uid == getuid() && st.st_gid == getgid());
    struct fuse_file_info fi = {.flags=O_RDONLY};
    CHECK(!nf_open("/file", &fi));
    unsigned char data[8195];
    CHECK(nf_read(NULL,(char *)data,sizeof(data),0,&fi) == sizeof(data));
    for (unsigned i=0;i<sizeof(data);i++) CHECK(data[i] == (unsigned char)(i*37));
    CHECK(!nf_release(NULL,&fi));
    CHECK(nf_getxattr("/file","user.binary",NULL,0) == 32);
    CHECK(nf_getxattr("/file","user.binary",(char *)data,32) == 32);
    for (unsigned i=0;i<32;i++) CHECK(data[i] == (unsigned char)(i*11));
    char link[32];
    CHECK(!nf_readlink("/sym",link,sizeof(link)) && !strcmp(link,"file"));
    CHECK(!nf_readlink("/sym",link,3) && !strcmp(link,"fi"));
}
static void exercise(void)
{
    struct fuse_file_info fi = {.flags=O_CREAT|O_EXCL|O_RDWR};
    CHECK(!nf_create("/file",0644,&fi));
    char data[8195];
    for (unsigned i=0;i<sizeof(data);i++) data[i]=(char)(i*37);
    CHECK(nf_write(NULL,data,sizeof(data),0,&fi) == sizeof(data));
    CHECK(!nf_flush(NULL,&fi) && !nf_flush(NULL,&fi));
    CHECK(!nf_release(NULL,&fi));
    CHECK(nf_create("/file",0600,&fi) == -EEXIST);
    CHECK(!nf_chmod("/file",0640,NULL));
    CHECK(!nf_chown("/file",(uid_t)-1,(gid_t)-1,NULL));
    struct timespec tv[2] = {{123456,0},{234567,0}};
    CHECK(!nf_utimens("/file",tv,NULL));
    tv[0].tv_nsec=tv[1].tv_nsec=UTIME_OMIT;
    CHECK(!nf_utimens("/file",tv,NULL));
    CHECK(!nf_mkdir("/dir",0755));
    CHECK(nf_mkdir("/dir",0755) == -EEXIST);
    struct fuse_file_info dir = {0};
    CHECK(nf_opendir("/file",&dir) == -ENOTDIR);
    CHECK(!nf_opendir("/dir",&dir));
    for(unsigned i=0;i<80;i++) {
        char name[32];snprintf(name,sizeof(name),"/dir/entry-%u",i);
        struct fuse_file_info e = {.flags=O_CREAT|O_RDWR};
        CHECK(!nf_create(name,0600,&e));CHECK(!nf_release(name,&e));
    }
    CHECK(!nf_unlink("/dir/entry-2"));
    struct listing l = {0};
    do { l.count=0;CHECK(!nf_readdir("/dir",&l,fill,l.next,&dir,0)); } while(l.count);
    CHECK(l.total == 81);
    CHECK(nf_rmdir("/dir") == -ENOTEMPTY);
    CHECK(nf_unlink("/dir") == -EISDIR);
    CHECK(!nf_mkdir("/empty",0700) && !nf_rmdir("/empty"));
    struct fuse_file_info temp = {.flags=O_CREAT|O_RDWR};
    CHECK(!nf_create("/gone",0600,&temp));
    CHECK(nf_write(NULL,"old",3,0,&temp) == 3);
    CHECK(!nf_unlink("/gone"));
    struct stat st;
    CHECK(nf_getattr("/gone",&st,NULL) == -ENOENT);
    CHECK(!nf_getattr(NULL,&st,&temp) && st.st_size == 3 && st.st_nlink == 0);
    CHECK(nf_read(NULL,data,3,0,&temp) == 3 && !memcmp(data,"old",3));
    CHECK(!nf_release(NULL,&temp));
    CHECK(!nf_link("/file","/next"));
    CHECK(!nf_create("/replace",0600,&temp));
    CHECK(nf_write(NULL,"old",3,0,&temp) == 3);
    CHECK(!nf_rename("/next","/replace",0));
    CHECK(nf_read(NULL,data,3,0,&temp) == 3 && !memcmp(data,"old",3));
    CHECK(!nf_release(NULL,&temp));
    CHECK(nf_rename("/file","/other",1) == -EOPNOTSUPP);
    CHECK(!nf_create("/append",0600,&temp));
    CHECK(nf_write(NULL,"A",1,0,&temp) == 1);CHECK(!nf_release(NULL,&temp));
    temp.flags=O_RDWR|O_APPEND;CHECK(!nf_open("/append",&temp));
    CHECK(nf_write(NULL,"B",1,0,&temp) == 1);
    CHECK(nf_read(NULL,data,2,0,&temp) == 2 && !memcmp(data,"AB",2));
    CHECK(!nf_truncate(NULL,10000,&temp));
    CHECK(nf_read(NULL,data,sizeof(data),2,&temp) == sizeof(data));
    for(unsigned i=0;i<sizeof(data);i++) CHECK(!data[i]);
    CHECK(!nf_release(NULL,&temp));
    temp.flags=O_RDWR|O_TRUNC;CHECK(!nf_open("/append",&temp));
    CHECK(!nf_getattr(NULL,&st,&temp) && st.st_size == 0);CHECK(!nf_release(NULL,&temp));
    unsigned char value[32];for(unsigned i=0;i<32;i++) value[i]=(unsigned char)(i*11);
    CHECK(!nf_setxattr("/file","user.binary",(char *)value,32,XATTR_CREATE));
    CHECK(nf_setxattr("/file","user.binary","bad",3,XATTR_CREATE) == -EEXIST);
    CHECK(nf_setxattr("/file","missing","bad",3,XATTR_REPLACE) == -ENODATA);
    CHECK(nf_getxattr("/file","user.binary",data,31) == -ERANGE);
    CHECK(!nf_setxattr("/file","user.empty",NULL,0,0));
    CHECK(nf_getxattr("/file","user.empty",NULL,0) == 0);
    CHECK(!nf_removexattr("/file","user.empty"));
    CHECK(!nvfuse_tx_begin(fs,NVFUSE_TX_METADATA,64));
    CHECK(!nf_setxattr("/file","user.binary","other",5,XATTR_REPLACE));
    CHECK(!nvfuse_tx_abort(fs));
    CHECK(!nf_symlink("file","/sym"));
    struct fuse_file_info opened[80] = {0};
    for(unsigned i=0;i<80;i++) CHECK(!nf_open("/file",&opened[i]));
    for(unsigned i=0;i<80;i++) CHECK(!nf_release(NULL,&opened[i]));
    verify();
}
int main(int argc,char **argv)
{
    CHECK(argc == 3);
    int create=!strcmp(argv[1],"create");
    fs=nvfuse_file_open(argv[2],create,create?256*1024*1024:0);CHECK(fs);
    if(create) { CHECK(!nf_owner("/"));exercise(); } else verify();
    CHECK(!nvfuse_file_close(fs));printf("PASS %s\n",argv[1]);return 0;
}
