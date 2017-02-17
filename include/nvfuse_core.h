/*
*	NVFUSE (NVMe based File System in Userspace)
*	Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>
*	First Writing: 30/10/2016
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*/
#include "nvfuse_config.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_types.h"
#include "nvfuse_bp_tree.h"
#include "list.h"
#include "rbtree.h"
#include "nvfuse_stat.h"

#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <string.h>

#ifndef __NVFUSE_HEADER_H__
#define __NVFUSE_HEADER_H__

/* NVFUSE SPECIAL SIGNATURE */
#define NVFUSE_SB_SIGNATURE	0x756c6673
#define NVFUSE_SS_SIGNATURE	0x709d2233

/* INITIAL BLOCK NUMBER */
#define INIT_NVFUSE_SUPERBLOCK_NO		0

/* INODE NUMBER */
#define RESRV0_INO		0
#define RESRV1_INO		1
#define ROOT_INO		2 /* root directory */
#define BLOCK_IO_INO	3 /* special inode number */
#define SS_INO			4 /* segment summary */
#define IFILE_INO		5 /* ifile */
#define SU_INO			6 /* segment usage */
#define DBITMAP_INO		7 /* data block bitmap */
#define IBITMAP_INO		8 /* inode bitmap*/
#define NUM_RESV_INO	9

/* INODE TYPE */
#define NVFUSE_TYPE_UNKOWN	1
#define NVFUSE_TYPE_SPECIAL	2
#define NVFUSE_TYPE_INODE		3
#define NVFUSE_TYPE_FILE		4
#define NVFUSE_TYPE_INDIRECT	5
#define NVFUSE_TYPE_DIRECTORY	6
#define NVFUSE_TYPE_TIME		7
#define NVFUSE_TYPE_BPTREE	8

/* DIR RELATED */
#define DIR_ENTRY_SIZE sizeof(struct nvfuse_dir_entry)
#define DIR_ENTRY_NUM (CLUSTER_SIZE/DIR_ENTRY_SIZE)
#define FNAME_SIZE (116)

#define DIR_EMPTY	(0)
#define DIR_USED	(1 << 1)
#define DIR_DELETED (1 << 2)

#define INODE_ENTRY_SIZE 128
#define INODE_ENTRY_NUM	(CLUSTER_SIZE/INODE_ENTRY_SIZE)

#define IFILE_ENTRY_SIZE INODE_ENTRY_SIZE
#define IFILE_ENTRY_NUM	INODE_ENTRY_NUM

/* ERROR STATUS */
#define	NVFUSE_ERROR		-1
#define	NVFUSE_SUCCESS		0
#define	NVFUSE_FORMATERR	1

#define READ 1
#define WRITE 0

#define DIRTY 1
#define CLEAN 0

#define FLUSH 1
#define UNFLUSH 0

#define LOCK 1
#define UNLOCK 0

#define READ_LOCK 1
#define WRITE_LOCK 0

#define SYNC 0
#define ASYNC 1

#define DATA 0
#define META 1

/* Various Macro Definition */

#define NVFUSE_CLUSTER_SIZE (CLUSTER_SIZE)
#define NVFUSE_CLUSTER_SIZE_BITS CLUSTER_SIZE_BITS
#define NVFUSE_SIZE_TO_BLK(x) ((s32)((s64)(x) >> NVFUSE_CLUSTER_SIZE_BITS))
#define NVFUSE_CLU_P_SEG(sb) (sb->sb_no_of_blocks_per_seg)
#define NVFUSE_CLUSTER_PER_SEGMENTS_BITS(s_bits,c_bits) (s_bits - c_bits)
#define NVFUSE_CLU_P_SEG_BITS(sb) NVFUSE_CLUSTER_PER_SEGMENTS_BITS(sb->sb_segment_size_bits, CLUSTER_SIZE_BITS)
#define NVFUSE_SEG_NUM(num_clu, clu_per_seg_bits) (num_clu >> clu_per_seg_bits)
#define NVFUSE_GET_SID(sb,clu) (clu >> NVFUSE_CLU_P_SEG_BITS(sb))
#define NVFUSE_GET_SEG_TO_CLU(sb,seg) (seg << NVFUSE_CLU_P_SEG_BITS(sb))
#define NVFUSE_NUM_CLU (DISK_SIZE >> CLUSTER_SIZE_BITS)

#define FALSE	0
#define TRUE	1

/* CAPACITY CALCULATION */
#define NVFUSE_TERA_BYTES ((u64)1024*1024*1024*1024)
#define NVFUSE_GIGA_BYTES (1024*1024*1024)
#define NVFUSE_MEGA_BYTES (1024*1024)
#define NVFUSE_KILO_BYTES (1024)

#define TB ((u64)1024*1024*1024*1024)
#define GB (1024*1024*1024)
#define MB (1024*1024)
#define KB (1024)

/* # OF MAX OPEN FILE */
#define START_OPEN_FILE	3 /* STDIN, STDOUT, STDERR*/
#define MAX_OPEN_FILE	16

#define NVFUSE_BP_TYPE_DATA 0
#define NVFUSE_BP_TYPE_DIR 1
#define NVFUSE_BP_TYPE_ETC 2

#define NVFUSE_BP_TYPE_BITS 2
#define NVFUSE_BP_HIGH_BITS 32
#define NVFUSE_BP_LOW_BITS 32
#define NVFUSE_BP_COLLISION_BITS 2

#define NVFUSE_MAX_BITS 32
#define NVFUSE_MAX_INODE_BITS 30
#define NVFUSE_MAX_DIR_BITS 32
#define NVFUSE_MAX_FILE_BITS 32

struct nvfuse_app_superblock {
	s32 asb_root_seg_id;
	s32 asb_free_inodes;
	s64 asb_free_blocks;
	s64 asb_no_of_used_blocks;
	s32 asb_core_id;
};

struct nvfuse_superblock_common {
	u32 sb_signature; //RDONLY	
	s64 sb_no_of_sectors;//RDONLY	
	s64 sb_no_of_blocks;//RDONLY	
	s64 sb_no_of_used_blocks; /* FS view*/

	u32 sb_no_of_inodes_per_seg;
	u32 sb_no_of_blocks_per_seg;

	u32 sb_root_ino;/* RDONLY */
	s32 sb_free_inodes; /* FS view */
	s64 sb_free_blocks; /* FS view */

	s32 sb_segment_num; /*RDONLY*/	

	u32 sb_umount;/* SYNC TIME*/
	s32 sb_mount_cnt;

	s32 sb_max_file_num;
	s32 sb_max_dir_num;
	s32 sb_max_inode_num;

	struct nvfuse_app_superblock asb;
};

/* Super Block Structure */
struct nvfuse_superblock{
	struct { /* Must be identical to nvfuse_super_common */
		u32 sb_signature; //RDONLY	
		s64	sb_no_of_sectors;//RDONLY	
		s64	sb_no_of_blocks;//RDONLY	
		s64	sb_no_of_used_blocks; /* FS view*/

		u32 sb_no_of_inodes_per_seg;
		u32 sb_no_of_blocks_per_seg;

		u32	sb_root_ino;/* RDONLY */
		s32 sb_free_inodes; /* FS view */
		s64 sb_free_blocks; /* FS view */

		s32	sb_segment_num; /*RDONLY*/	

		u32	sb_umount;/* SYNC TIME*/
		s32 sb_mount_cnt;

		s32	sb_max_file_num;
		s32	sb_max_dir_num;
		s32	sb_max_inode_num;

		struct nvfuse_app_superblock asb;
	};

	struct {
		struct control_plane_context *sb_control_plane_ctx;		
		s32 sb_control_plane_buffer_size;
		s32	sb_next_segment; /* SYNC TIME or BACKGROUND CLEANING*/
		s32	sb_cur_segment; /* SYNC TIME or BACKGROUND CLEANING*/
		s32	sb_cur_clu; /* SYNC TIME or BACKGROUND CLEANING*/
	
		s32 sb_last_allocated_ino;
		s32 sb_last_allocated_sid;		
		s32 sb_last_allocated_sid_by_ino;		
		struct nvfuse_io_manager *io_manager;

		struct nvfuse_segment_summary *sb_ss; /* SYNC TIME*/
		struct nvfuse_handle *sb_nvh;
		
		s32 sb_sb_cur;
		s32 sb_sb_flush;
		
		/* block buffer manager */
		struct nvfuse_buffer_manager *sb_bm;

		/* inode context cache */
		struct nvfuse_ictx_manager *sb_ictxc;
						
		struct nvfuse_file_table *sb_file_table; /* INCLUDING FINE GRAINED LOCK */
		//pthread_mutex_t sb_file_table_lock; /* COARSE LOCK */

		struct timeval sb_last_update;	/* SUPER BLOCK in memory UPDATE TIME */
		struct timeval sb_sync_time; /* LAST SYNC TIME */

		//pthread_mutex_t sb_iolock;

		//pthread_mutex_t sb_request_lock;
		s32 sb_request_count;

		s32 sb_dirty_sync_policy;

		struct list_head sb_seg_list;
		s32 sb_seg_list_count;
		struct list_head *sb_seg_search_ptr_for_inode;
		struct list_head *sb_seg_search_ptr_for_data;

		//struct list_head *sb_seg_cur_ptr; /* hint */

		s32 sb_is_primary_process;
		
		/* Read and Write statistics*/
		u64 sb_write_blocks;
		u64 sb_read_blocks;
		u64 sb_write_ios;
		u64 sb_read_ios;
		u64 sb_meta_write_blocks;
		u64 sb_meta_read_blocks;
		u64 sb_meta_write_ios;
		u64 sb_meta_read_ios;
		u64 sb_bp_write_blocks;

		struct timeval sb_time_start;
		struct timeval sb_time_end;
		struct timeval sb_time_total;
		
		u64 bp_set_index_tsc;
		u64 bp_set_index_count;

		u64 nvme_io_tsc;
		u64 nvme_io_count;

		/* perf stat ipc */
		union perf_stat perf_stat_ipc;
		/* buffer head mempool */
		struct spdk_mempool *bh_mempool;
		/* buffer cache mempool */
		struct spdk_mempool *bc_mempool; /* allocated for primary core */
		/* bptree mempool */
		struct spdk_mempool *bp_mempool[BP_MEMPOOL_NUM];
		/* seg node mempool*/
		struct spdk_mempool *seg_mempool; /* allocated for primary core */
	};
};

struct seg_node{
 struct list_head list;
 u32 seg_id;
};

struct nvfuse_file_table{
	inode_t	ino;
	s64	size;
	s32	used;
	nvfuse_off_t prefetch_cur;
	nvfuse_off_t rwoffset;
	s32 flags;
	//pthread_mutex_t ft_lock;
};

#define MAX_FILES_PER_DIR (0x7FFFFFFF)

struct nvfuse_dir_entry{	
	inode_t	d_ino;
	u32	d_flag;
	u32	d_version;
	s8	d_filename[FNAME_SIZE];	
};

#define NVFUSE_SUPERBLOCK_OFFSET  0
#define NVFUSE_SUPERBLOCK_SIZE    1
#define NVFUSE_SUMMARY_OFFSET     1
#define NVFUSE_IBITMAP_OFFSET     2
#define NVFUSE_IBITMAP_SIZE       1
#define NVFUSE_DBITMAP_OFFSET     (NVFUSE_IBITMAP_OFFSET+NVFUSE_IBITMAP_SIZE)
#define NVFUSE_DBITMAP_SIZE       1

struct nvfuse_segment_summary{
	u32 ss_magic;
	u32 ss_owner;
	u32 ss_id;
	u32 ss_seg_start;
	u32 ss_summary_start;
	u32 ss_max_inodes;
	u32 ss_max_blocks;
	u32 ss_ibitmap_start;
	u32 ss_ibitmap_size;
	u32 ss_dbitmap_start;
	u32 ss_dbitmap_size;
	u32 ss_itable_start;
	u32 ss_itable_size;
	u32 ss_dtable_start;
	u32 ss_dtable_size;

	u32 ss_free_inodes;
	u32 ss_free_blocks;

	u32 ss_next_block;	
};

#define DIRECT_BLOCKS       11
#define INDIRECT_BLOCKS     11
#define DINDIRECT_BLOCKS    12
#define TINDIRECT_BLOCKS    13
#define INDIRECT_BLOCKS_LEVEL 4

#define PTRS_PER_BLOCK		(CLUSTER_SIZE/sizeof(u32))
#define PTRS_PER_BLOCK_BITS	10

#define MAX_FILE_SIZE ((s64)DIRECT_BLOCKS * CLUSTER_SIZE + \
						(s64)(1 << PTRS_PER_BLOCK_BITS) * CLUSTER_SIZE + \
						(s64)(1 << (PTRS_PER_BLOCK_BITS * 2)) * CLUSTER_SIZE + \
						(s64)(1 << (PTRS_PER_BLOCK_BITS * 3)) * CLUSTER_SIZE)

struct nvfuse_inode{	
	inode_t	i_ino; //4
	u32	i_type; //8
	u32 i_bpino;
	s64	i_size;	//16	
	u32	i_version; //20	
	u32	i_deleted; // 24
	u32	i_links_count;	/* Links count */ //28
	u32	i_ptr;		/* for directory entry */ //32	
	u32	i_atime;	/* Access time */ //36
	u32	i_ctime;	/* Inode change time */ //40
	u32	i_mtime;	/* Modification time */ //44
	u32	i_dtime;	/* Deletion Time */ //48
	u16	i_gid;		/* Low 16 bits of Group Id */ //50
	u16	i_uid;		/* Low 16 bits of Owner Uid */	//52 	
	u16	i_mode;		/* File mode */ //54
	u32 resv1[1]; //64
	u32 i_blocks[TINDIRECT_BLOCKS + 1];
	u32 resv2[1];	
};

struct nvfuse_inode_ctx{
	inode_t ictx_ino;
	struct list_head ictx_cache_list;   /* cache list */
	struct hlist_node ictx_hash;		/* hash list*/

	struct nvfuse_inode *ictx_inode;
	struct nvfuse_buffer_head *ictx_bh;

	struct list_head ictx_meta_bh_head;
	struct list_head ictx_data_bh_head;	

#ifdef USE_RBNODE
    struct rb_root ictx_meta_bh_rbroot;	
    struct rb_root ictx_data_bh_rbroot;
#endif

	s32 ictx_meta_dirty_count;
	s32 ictx_data_dirty_count;

	s32 ictx_type;
	s32 ictx_status;
	s32 ictx_ref;
};

#if NVFUSE_OS == NVFUSE_OS_WINDOWS
struct iovec{
	s8 *iov_base;
	u32 iov_len;
};
#endif 

#define nvfuse_dirname fat_dirname
#define nvfuse_filename fat_filename

struct control_plane_context;
struct nvfuse_params {
	s8 appname[128];
	s32 cpu_core_mask;
	s32 buffer_size; /* in MB units */
	s32 qdepth;
	s32 need_format;
	s32 need_mount;
};

/* IPC Ring Queue Name */
#define NVFUSE_SEC_TO_PRI_NAME "SEC_2_PRI"
#define NVFUSE_PRI_TO_SEC_NAME "PRI_2_SEC"
/* Mempool Name */
#define NVFUSE_MSG_POOL_NAME "MSG_POOL"

struct nvfuse_ipc_context {
	/* IPC Ring Queue Name */
	char _SEC_2_PRI[SPDK_NUM_CORES][64];
	char _PRI_2_SEC[SPDK_NUM_CORES][64];
	
	/* IPC Ring Structure */
	struct rte_ring *send_ring[SPDK_NUM_CORES];
	struct rte_ring *recv_ring[SPDK_NUM_CORES];
	
	/* ID Gen Ring */
	char ID_GEN[64];
	struct rte_ring *id_gen;

	/* MSG Mempool Name*/
	char _MSG_POOL[64];;

	/* MSG Mempool Structure */
	struct rte_mempool *message_pool;

	/* Stat Ring and Mempool */
	struct rte_ring *stat_ring[MAX_NUM_STAT];
	struct rte_mempool *stat_pool[MAX_NUM_STAT];

	int my_channel_id;
};

struct nvfuse_handle {
	struct nvfuse_superblock nvh_sb;
	struct nvfuse_io_manager nvh_iom;
	struct nvfuse_params nvh_params;
	struct nvfuse_ipc_context nvh_ipc_ctx;	

	u32 nvh_cwd_ino;
	u32 nvh_root_dir;
	u32 nvh_root_ino;
	u32 nvh_mounted;
};

s32 error_msg(s8 *str);
s32 fat_dirname(const s8 *path, s8 *dest);
s32 fat_filename(const s8 *path, s8 *dest);
void nvfuse_dir_hash(s8 *filename, u32 *hash, u32 *hash2);
int nvfuse_read_block(char *buf, unsigned long block, struct nvfuse_io_manager *io_manager);

struct nvfuse_superblock * nvfuse_read_super(struct nvfuse_handle *nvh);
void nvfuse_release_super(struct nvfuse_superblock *sb);


void nvfuse_print_inode(struct nvfuse_inode *inode, s8 *str);
struct nvfuse_inode_ctx *nvfuse_read_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino);

inode_t nvfuse_alloc_new_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);

void nvfuse_release_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s32 dirty);
s32 nvfuse_relocate_delete_inode(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx);
void nvfuse_mark_inode_dirty(struct nvfuse_inode_ctx *ictx);

void nvfuse_free_inode_size(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, s64 size);
s32 ext2fs_set_bit(u32 nr,void * addr);
s32 ext2fs_clear_bit(u32 nr, void * addr);
s32 ext2fs_test_bit(u32 nr, const void * addr);
s32 nvfuse_set_dir_indexing(struct nvfuse_superblock *sb,struct nvfuse_inode *inode,s8 *filename,u32 offset);
s32 nvfuse_get_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode,s8 *filename,bitem_t *offset);
s32 nvfuse_get_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode,s8 *filename,bitem_t *offset);
s32 nvfuse_del_dir_indexing(struct nvfuse_superblock *sb, struct nvfuse_inode *inode,s8 *filename);
s32 nvfuse_sync_dirty_data(struct nvfuse_superblock *sb, struct list_head *head, s32 num_blocks);

s32 nvfuse_umount(struct nvfuse_handle *nvh);
void nvfuse_copy_mem_sb_to_disk_sb(struct nvfuse_superblock *disk, struct nvfuse_superblock *memory);
void nvfuse_copy_disk_sb_to_sb(struct nvfuse_superblock *memory, struct nvfuse_superblock *disk);
s32 nvfuse_is_sb(s8 *buf);
s32 nvfuse_is_ss(s8 *buf);
void *allocate_aligned_buffer(size_t size);
u32 get_part_size(s32 fd);
u32 get_sector_size(s32 fd);
u64 get_no_of_sectors(s32 fd);

s32 nvfuse_dir(struct nvfuse_handle *nvh);

s32 nvfuse_allocate_open_file_table(struct nvfuse_superblock *sb);
s32 nvfuse_chmod(struct nvfuse_handle *nvh, inode_t par_ino,s8 *filename,mode_t mode);
s32 nvfuse_path_open(struct nvfuse_handle *nvh, s8 *path, s8 *filename, struct nvfuse_dir_entry *get);
s32 nvfuse_path_open2(struct nvfuse_handle *nvh, s8 *path, s8 *filename, struct nvfuse_dir_entry *get);
s32 nvfuse_seek(struct nvfuse_superblock *sb, struct nvfuse_file_table *of, s64 offset, s32 position);
s32 nvfuse_link(struct nvfuse_superblock *sb, u32 newino, s8 *new_filename, s32 ino);
s32 nvfuse_rm_direntry(struct nvfuse_superblock *sb, inode_t par_ino,s8 *name,u32 *ino);
u32 nvfuse_get_pbn(struct nvfuse_superblock *sb, struct nvfuse_inode_ctx *ictx, inode_t ino, lbno_t offset);

s32 nvfuse_release_bh(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, s32 tail, s32 dirty);

s32  nvfuse_mkfile(struct nvfuse_handle *nvh, s8 *str,s8 *ssize);
s32 nvfuse_cd(struct nvfuse_handle *nvh, s8 *str);
void nvfuse_test(struct nvfuse_handle *nvh);
s32 nvfuse_rdfile(struct nvfuse_handle *nvh, s8 *str);
bkey_t *nvfuse_make_key(inode_t ino, lbno_t lbno,bkey_t *key,u32 type);
u64 *nvfuse_make_pbno_key(inode_t ino, lbno_t lbno, u64 *key, u32 type);
s32 nvfuse_mount(struct nvfuse_handle *nvh);

s32 nvfuse_truncate_ino(struct nvfuse_superblock *sb, inode_t ino, s64 trunc_size);
s32 nvfuse_truncate(struct nvfuse_superblock *sb, inode_t par_ino, s8 *filename, nvfuse_off_t trunc_size);
u32 nvfuse_create_bptree(struct nvfuse_superblock *sb, struct nvfuse_inode *inode);
u32 nvfuse_alloc_dbitmap(struct nvfuse_superblock *sb, u32 seg_id, u32 *alloc_blks, u32 num_blocks);
u32 nvfuse_free_dbitmap(struct nvfuse_superblock *sb, u32 seg_id, nvfuse_loff_t offset, u32 count);
void nvfuse_free_blocks(struct nvfuse_superblock *sb, u32 block_to_delete, u32 count);
s32 nvfuse_scan_superblock(struct nvfuse_superblock *cur_sb);
s32 nvfuse_lseek(struct nvfuse_handle *nvh, s32 fd, u32 offset, s32 position);
s32 nvfuse_format(struct nvfuse_handle *nvh);
u32 nvfuse_get_free_blocks(struct nvfuse_superblock *sb, u32 seg_id);

void nvfuse_check_flush_dirty(struct nvfuse_superblock *sb, s32 force);

void nvfuse_inc_free_inodes(struct nvfuse_superblock *sb, inode_t ino);

#define nvfuse_lock()	
#define nvfuse_unlock() 

void nvfuse_lock_init(void);
void nvfuse_lock_exit(void);

s32 nvfuse_dir_is_invalid(struct nvfuse_dir_entry *dir);

s32 nvfuse_make_jobs(struct io_job **jobs, int numjobs);

void nvfuse_release_ibitmap(struct nvfuse_superblock *sb, u32 seg_id, u32 ino);
s32 nvfuse_is_directio(struct nvfuse_superblock *sb, s32 fid);

s32 nvfuse_alloc_container_from_primary_process(struct nvfuse_handle *nvh, s32 type);
s32 nvfuse_dealloc_container_from_primary_process(struct nvfuse_superblock *sb, u32 seg_id);
u32 nvfuse_get_curr_seg_id(struct nvfuse_superblock *sb, s32 is_inode);
u32 nvfuse_get_next_seg_id(struct nvfuse_superblock *sb, s32 is_inode);
void nvfuse_move_curr_seg_id(struct nvfuse_superblock *sb, s32 seg_id, s32 is_inode);

s32 nvfuse_add_seg(struct nvfuse_superblock *sb, u32 seg_id);
s32 nvfuse_remove_seg(struct nvfuse_superblock *sb, u32 seg_id);

void nvfuse_update_sb_with_ss_info(struct nvfuse_superblock *sb, s32 seg_id, s32 is_root_container, s32 increament);

s32 nvfuse_check_free_inode(struct nvfuse_superblock *sb);
s32 nvfuse_check_free_block(struct nvfuse_superblock *sb, u32 num_blocks);
void nvfuse_print_seg_list(struct nvfuse_superblock *sb);
void nvfuse_update_owner_in_ss(struct nvfuse_superblock *sb, s32 seg_id);

#endif /* NVFUSE_HEADER_H */