/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVBRIDGE_H
#define NVBRIDGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int nvdb_start(const char*,const char*,int);
int nvdb_stop(void);
int nvdb_open(const char*,int);
int nvdb_close(int);
int nvdb_read(int,void*,size_t,uint64_t);
int nvdb_write(int,const void*,size_t,uint64_t);
int nvdb_truncate(int,uint64_t);
int nvdb_sync(int);
int nvdb_stat(const char*,uint64_t*,uint64_t*,unsigned*);
int nvdb_mkdir(const char*);
int nvdb_unlink(const char*);
int nvdb_rmdir(const char*);
int nvdb_rename(const char*,const char*);
int nvdb_link(const char*,const char*);
int nvdb_list(const char*,char***,size_t*);
void nvdb_profile(const char*);
#ifdef __cplusplus
}
#endif
#endif
