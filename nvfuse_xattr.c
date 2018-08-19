/*
*       NVFUSE Extended Attribute 
*       Copyright (C) 2017 Hankeun Son <hankeun.son@sk.com>
*       First Writing: 30/05/2017
*
*       This code based NVFUSE (NVMe based File System in Userspace)
*       Copyright (C) 2016 Yongseok Oh <yongseok.oh@sk.com>

* xattr consolidation Copyright (c) 2004 James Morris <jmorris@redhat.com>,
* Red Hat Inc.
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

#include "nvfuse_xattr.h"

//#define TEST_DEBUG_CODE
//#define PRINT_SCREEN
//#define BUFFER_FLUSH

#define NVFUSE_XATTR_MAX_SIZE 3968 //MAX EA space size limits 3972
#define NVFUSE_XATTR_NAME_MAX_LEN 256 // MAX name length limits 256
#define NVFUSE_XATTR_VALUE_MAX_LEN 512	// Max value length limits 512
#define XATTR_ENTRY(ptr)	((struct nvfuse_xattr_entry *)(ptr))
#define XATTR_FIRST_ENTRY(entry) (XATTR_ENTRY(entry))
#define ENTRY_SIZE(entry) ((sizeof(u32) + sizeof(u32) +\
                          entry->e_name_len + 1 + entry->e_value_size) + 1)
#define XATTR_NEXT_ENTRY(entry) ((struct nvfuse_xattr_entry *)((char *)(entry) +\
                        ENTRY_SIZE(entry)))
#define IS_XATTR_LAST_ENTRY(entry) (*(u32 *)(entry) == 0)
#define list_for_each_xattr(entry, addr) \
                for (entry = XATTR_FIRST_ENTRY(addr);\
                                !IS_XATTR_LAST_ENTRY(entry);\
                                entry = XATTR_NEXT_ENTRY(entry))

/*
 * nvfuse_set_xattr()
 *
 * GOAL: Write an extended attribute into the inode. 
 * NOW: Sequential write new extended attribute pairs(name, value)
 *      with various length.
 *      To REPLACE as same name with different value_size, 
 *      we REMOVE pre pair(name,value) and APPEND new pair(name, value').
 *      Check to available space for extended attribute.
 */
s32 nvfuse_set_xattr(struct nvfuse_handle *nvh, const char *path,
		const char *name, const char *value)
{	
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	struct nvfuse_xattr_entry *last, *here;
	void *base_addr;
	u32 name_len;
	u32 value_len, free;
	s32 res = 0;

	#ifdef TEST_DEBUG_CODE
	printf("set\n");
	#endif 

	/* error for NULL input */
	if (name == NULL || value == NULL) {
		#ifdef PRINT_SCREEN
		printf("input error\n");
		#endif
		res = -1;
		goto RET;
	}
	
	/* error for invalid length name*/
	name_len = strlen(name);
	#ifdef TEST_DEBUG_CODE
	printf("name_len: %u\t", name_len);
	#endif 
	if (name_len > NVFUSE_XATTR_NAME_MAX_LEN || name_len < 1) {	// 1-256 length
		#ifdef PRINT_SCREEN
		printf(" %u: invalid name length\n", name_len);
		#endif
		res = -1;
		goto RET;
	}

	/* error for invalid lengh value */ 
	value_len = strlen(value);
	#ifdef TEST_DEBUG_CODE
	printf("value_len: %u\t",value_len);
	#endif 
	if(value_len > NVFUSE_XATTR_VALUE_MAX_LEN || value_len < 1) {	// 1-512 length
		#ifdef PRINT_SCREEN
		printf(" %u: invalid value length\n", value_len);
		#endif
		res = -1;
		goto RET;
	}

	/* Get inode */
	 if (strcmp(path, "/") == 0) {
                res = -1;
		goto RET;
	}
	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
		goto RET;
	}

	sb = nvfuse_read_super(nvh);
	if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
		res = -1;
		goto RET;
	}

	inode = ictx->ictx_inode;

	/* Write new entry*/
	if(value) {
		char *pval;
		bool is_found = 0;

		// Get xattr address in Inode to extended attribute space base_addr.
		base_addr = (void *)&(inode->xattr[0]);
		list_for_each_xattr(here, base_addr) {
		if (here->e_name_len != name_len)
				continue;
			if (!memcmp(here->e_name, name, name_len)) {
				is_found = 1;
				break;
			}
		}

		// create xattr entry
		if(!is_found) {
			last = here;
	
			// check free space  
			free = (u32)(NVFUSE_XATTR_MAX_SIZE - ((u32)last - (u32)base_addr));

			#ifdef TEST_DEBUG_CODE
			printf("free space: %u\n", free);	//
			#endif
			if(sizeof(u32) + sizeof(u32) + name_len + 1 + value_len + 1 > free) {
				#ifdef PRINT_SCREEN
				printf("insufficienty xattr free space to create\n");	//
				#endif
	                        res = -1;
				nvfuse_release_inode(sb, ictx, NVF_CLEAN);
				goto RELEASE_SUPER;
			}

		}

		// replace xattry entry
		else {
			last = base_addr;

			/* Find last addr */			
			while (!IS_XATTR_LAST_ENTRY(last))
				last = XATTR_NEXT_ENTRY(last);

			// check free space  
			free = (u32)(NVFUSE_XATTR_MAX_SIZE - ((u32)last - (u32)base_addr));
					
			#ifdef TEST_DEBUG_CODE
			printf("free space: %u\n", free);	//
			#endif
			if(value_len - here->e_value_size > free) {
				#ifdef PRINT_SCREEN
				printf("insufficienty xattr free space to replace\n");	//
				#endif
                                res = -1;
				nvfuse_release_inode(sb, ictx, NVF_CLEAN);
				goto RELEASE_SUPER;
			}

			/* Remove Entry */
			if(is_found) {
				struct nvfuse_xattr_entry *next = XATTR_NEXT_ENTRY(here);
				int shrinksize = ENTRY_SIZE(here);
	
				memmove(here, next, (char *)last - (char *)next);
				last = XATTR_ENTRY((char *)last - shrinksize);
				memset(last, 0, shrinksize);
			}
		}

		#ifdef TEST_DEBUG_CODE
		printf("last: %u\n", (unsigned int)last); //
		#endif

		// Copy name, value 
		last->e_name_len = name_len;
		memcpy(last->e_name, name, name_len);
		pval = last->e_name + name_len + 1;
		memcpy(pval, value, value_len);
		last->e_value_size = value_len;
		#ifdef TEST_DEBUG_CODE
//		printf("xattr space\n");
//		for(int i = 0; i<300; i++){
//			printf("%2d:%c ", i+80, (char)(inode->xattr[i+3880]));
//		}
//		printf("\n");
		#endif

	nvfuse_release_inode(sb, ictx, DIRTY);

	#ifdef BUFFER_FLUSH
	// temp code for flush
	nvfuse_check_flush_dirty(sb, 1);
	#endif


RELEASE_SUPER:

	nvfuse_release_super(sb);
	}

RET:

        return res;
}

/*
 * nvfuse_remove_xattr()
 *
 * GOAL:  Remove an extended attribute into the xattr list. 
 * NOW:	  Find a pair
 *	  Delete the pair
 *	  Move forward other pair post deleted pair
 */
s32 nvfuse_remove_xattr(struct nvfuse_handle *nvh, const char *path,
		const char *name)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	struct nvfuse_xattr_entry *last, *here;
	void *base_addr;
	u32 name_len;
	bool is_found = 0;
	s32 res = 0;

	#ifdef TEST_DEBUG_CODE
	printf("remove\n");
	#endif 

	/* error for NULL input */
	if (name == NULL) {
		printf("input error\n");
		res = -1;
		goto RET;
	}
	
	/* error for over length name*/
	name_len = strlen(name);
	if (name_len > NVFUSE_XATTR_NAME_MAX_LEN || name_len < 1) {	// 1-256 length
		#ifdef PRINT_SCRENN
		printf(" %u: invalid name length\n", name_len);
		#endif
		res = -1;
		goto RET;
	}

	/* Get inode */
	if (strcmp(path, "/") == 0) {
                res = -1;
		goto RET;
	}

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
		goto RET;
	}

	sb = nvfuse_read_super(nvh);
	if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
		res = -1;
		goto RET;
	}

	inode = ictx->ictx_inode;

	// Get xattr address in Inode to extended attribute space base_addr.
	base_addr = (void *)&(inode->xattr[0]);

	/* find entry with wanted name */
	list_for_each_xattr(here,base_addr) {
		if (here->e_name_len != name_len)
			continue;
		if (!memcmp(here->e_name, name, name_len)) {
			is_found = 1;
			break;
		}
	}

	if(is_found != 1) {
		res = -1;
		nvfuse_release_inode(sb, ictx, NVF_CLEAN);
		goto RELEASE_SUPER;
	}

	last = here;

	/* Find last addr */			
	while (!IS_XATTR_LAST_ENTRY(last)) {
		last = XATTR_NEXT_ENTRY(last);
	}
	/* Remove Entry */
	if(last!=here) {
		struct nvfuse_xattr_entry *next = XATTR_NEXT_ENTRY(here);
		int shrinksize = ENTRY_SIZE(here);

		memmove(here, next, (char *)last - (char *)next);
		last = XATTR_ENTRY((char *)last - shrinksize);
		memset(last, 0, shrinksize);
	}

	nvfuse_release_inode(sb, ictx, DIRTY);

	#ifdef BUFFER_FLUSH
	// temp code for flush
	nvfuse_check_flush_dirty(sb, 1);
	#endif


RELEASE_SUPER:

	nvfuse_release_super(sb);

RET:

        return res;
}

/*
 * nvfuse_get_xattr()
 *
 * GOAL: Copy an extended attribute into the buffer provided. 
 *
 */
s32 nvfuse_get_xattr(struct nvfuse_handle *nvh, const char *path,
		const char *name, char *buffer, size_t buf_size)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	struct nvfuse_xattr_entry *last;
	void *base_addr;
	u32 name_len;
	bool is_found = 0;
	s32 res = 0;

	#ifdef TEST_DEBUG_CODE
	printf("get\n");
	#endif 

	/* error for NULL input */
	if (name == NULL) {
		#ifdef PRINT_SCREEN
		printf("input error\n");
		#endif
		res = -1;
		goto RET;
	}
	
	/* error for over length name*/
	name_len = strlen(name);
	if (name_len > NVFUSE_XATTR_NAME_MAX_LEN || name_len < 1) {	// 1-256 length
		#ifdef PRINT_SCREEN
		printf(" %u: invalid name length\n", name_len);
		#endif
		res = -1;
		goto RET;
	}

	/* Get inode */
	 if (strcmp(path, "/") == 0) {
                res = -1;
		goto RET;
         }

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0) 
		return res;

	if (dir_entry.d_ino == 0) {
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
		goto RET;
	}

	sb = nvfuse_read_super(nvh);
	if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
		res = -1;
		goto RET;
	}
	
	inode = ictx->ictx_inode;
	
	// Get xattr address in Inode to extended attribute space base_addr.
	base_addr = (void *)&(inode->xattr[0]);
	list_for_each_xattr(last, base_addr) {
		if (last->e_name_len != name_len)
			continue;
		if (!memcmp(last->e_name, name, name_len)) {
			is_found = 1;
			break;
		}
	}

	if(is_found != 1) {
		res = -1;
		goto RELEASE;
	}

	if(ENTRY_SIZE(last) > buf_size){
		res = ERANGE;
		goto RELEASE;
	}
	memset(buffer, 0x00, buf_size);
	if (buffer) {
		memcpy(buffer, last->e_name, ENTRY_SIZE(last) - sizeof(u32) - sizeof(u32));
	}

	#ifdef PRINT_SCREEN
	printf("name: %s\n", last->e_name);
//	printf("name addr: %u\n",(unsigned int)&last->e_name);
	printf("value: %s\n", last->e_name + 1 + last->e_name_len);
	#endif

RELEASE:

	nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	nvfuse_release_super(sb);

RET:

        return res;
}

/*
 * nvfuse_list_xattr()
 *
 * GOAL: Copy a list of extended attribute names into the buffer provided. 
 *
 */
s32 nvfuse_list_xattr(struct nvfuse_handle *nvh, const char *path,
		char *buffer, size_t buf_size)
{
	struct nvfuse_dir_entry dir_entry;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_superblock *sb;
	char filename[FNAME_SIZE];
	struct nvfuse_xattr_entry *last;
	void *base_addr;
	size_t rest = buf_size;
	s32 res = 0;

	#ifdef TEST_DEBUG_CODE
	printf("list\n");
	#endif 

	/* Get inode */
	 if (strcmp(path, "/") == 0) {
                res = -1;
		goto RET;
	}

	res = nvfuse_path_resolve(nvh, path, filename, &dir_entry);
	if (res < 0)
		return res;

	if (dir_entry.d_ino == 0) {
		printf(" %s: invalid path\n", __FUNCTION__);
		res = -1;
		goto RET;
	}

	sb = nvfuse_read_super(nvh);
	if (nvfuse_lookup(sb, &ictx, &dir_entry, filename, dir_entry.d_ino) < 0) {
		res = -1;
		goto RET;
	}

	inode = ictx->ictx_inode;
	
	// Get xattr address in Inode to extended attribute space base_addr.
	base_addr = (void *)&(inode->xattr[0]);

	/* Find entry*/
	memset(buffer, 0x00, buf_size);
	list_for_each_xattr(last, base_addr) {
				
		if(last->e_name_len + 1 > rest) {
			res = -1;
			goto RELEASE;

		}
		memcpy(buffer, last->e_name, last->e_name_len + 1);

		if(!IS_XATTR_LAST_ENTRY(last)) 
			buffer += last->e_name_len + 1;
		rest -= last->e_name_len + 1;				

		#ifdef PRINT_SCREEN
		printf("name: %s\n", last->e_name);
//		printf("value: %s\n",last->e_name + 1 + last->e_name_len);
		#endif
	}

RELEASE:		

	nvfuse_release_inode(sb, ictx, NVF_CLEAN);
	nvfuse_release_super(sb);

RET:

        return res;
}
