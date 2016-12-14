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


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#ifndef __linux__
	#include <windows.h>
#endif

#include "nvfuse_core.h"
#include "nvfuse_bp_tree.h"
#include "nvfuse_buffer_cache.h"
#include "nvfuse_indirect.h"

static u32 bp_memalloc_size = 0;

void *bp_malloc(u32 size) {
	bp_memalloc_size++;
	return malloc(size);
}

void bp_free(void *ptr) {
	bp_memalloc_size--;
	free(ptr);
}

master_node_t *bp_init_master()
{
	master_node_t *master;

	// init master node 
	master = (master_node_t *)bp_malloc(sizeof(master_node_t));
	if (master == NULL) {
		printf(" Error: malloc()\n");
	}

	memset(master, 0x00, sizeof(master_node_t));

	master->alloc = bp_alloc_node;
	master->insert = bp_insert_key_tree;
	master->update = bp_update_key_tree;
	master->search = search_data_node;
	master->range_search = rsearch_data_node;
	//master->range_search_rb = rsearch_data_node_rb;
	master->remove = bp_remove_key;
	master->get_pair = get_pair_tree;
	master->release = bp_release_node;
	master->release_bh = bp_release_bh;
	master->read = bp_read_node;
	master->write = bp_write_node; 
	master->push = stack_push;
	master->pop = stack_pop;
	master->dealloc= bp_dealloc_bitmap;	
	master->m_root = 0;
	
	master->m_node_size = CLUSTER_SIZE;
	master->m_fanout = (master->m_node_size - BP_NODE_HEAD_SIZE) / (BP_PAIR_SIZE);
	if (master->m_fanout % 2 == 0)
		master->m_fanout -= 1;
	
	pthread_mutex_init(&master->m_big_lock, NULL);
	
	return master;
}

void bp_deinit_master(master_node_t *master)
{
	nvfuse_release_inode(master->m_sb, master->m_ictx, 
						test_bit(&master->m_ictx->ictx_status, BUFFER_STATUS_DIRTY) ? 1 : 0);

	pthread_mutex_destroy(&master->m_big_lock);
	bp_free((void *)master);
}

int bp_alloc_master(struct nvfuse_superblock *sb, master_node_t *master) 
{
	inode_t ino;
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	struct nvfuse_buffer_head *bh;
	s32 ret;
	ictx = nvfuse_alloc_ictx(sb);
	if (ictx == NULL)
		return -1;

	ino = nvfuse_alloc_new_inode(sb, ictx);
	if (ino == 0)
	{
		printf(" It runs out of free inodes.");
		return -1;
	}

	ictx = nvfuse_read_inode(sb, ictx, ino);
	nvfuse_insert_ictx(sb, ictx);
	set_bit(&ictx->ictx_status, BUFFER_STATUS_DIRTY);

	inode = ictx->ictx_inode;
	if (!inode) {
		printf(" inode allocation for b+tree master node failed \n");
		return -1;
	}

	inode->i_type = NVFUSE_TYPE_BPTREE;
	inode->i_size = 0;
	inode->i_links_count = 1;

	master->m_ino = inode->i_ino;
	master->m_sb = sb;
	master->m_alloc_block = 1; /* allocation of root node*/

	ret = nvfuse_get_block(sb, ictx, inode->i_size >> CLUSTER_SIZE_BITS, 1/* num block */, NULL, NULL, 1);
	if (ret)
	{
		printf(" data block allocation fails.");
		return NVFUSE_ERROR;
	}
	bh = nvfuse_get_new_bh(sb, ictx, inode->i_ino, inode->i_size >> CLUSTER_SIZE_BITS, NVFUSE_TYPE_META);
	nvfuse_release_bh(sb, bh, INSERT_HEAD, DIRTY);
	assert(inode->i_size < MAX_FILE_SIZE);
	inode->i_size += CLUSTER_SIZE;
	
	master->m_ictx = ictx;
	nvfuse_mark_inode_dirty(ictx);

	//nvfuse_release_inode(sb, ictx, DIRTY);

	master->m_ictx = ictx;
		
	return 0;
}

void bp_init_root(master_node_t *master)
{
	struct nvfuse_inode_ctx *ictx;
	struct nvfuse_inode *inode;
	index_node_t *root;
	struct nvfuse_buffer_head *bh;
	offset_t new_bno;

	/* allocation of root node block */
	//ictx = nvfuse_read_inode(master->m_sb, NULL, master->m_ino);
	ictx = master->m_ictx;
	inode = ictx->ictx_inode;

	new_bno = bp_alloc_bitmap(master, ictx);
	bh = nvfuse_get_bh(master->m_sb, ictx, master->m_ino, new_bno, READ, NVFUSE_TYPE_META);

	root = (index_node_t *)B_dALLOC(master, master->m_root, ALLOC_READ);
	root->i_root = 1;
	root->i_offset = 1;
	root->i_flag = DATA_FLAG;
	root->i_status = INDEX_NODE_USED;
	root->i_bh = bh;
	root->i_buf = bh->bh_buf;
	master->m_root = root->i_offset;

	B_WRITE(master, root, root->i_offset);	
	B_RELEASE_BH(master, root->i_bh);
	B_RELEASE(master, root);
	//nvfuse_release_inode(master->m_sb, ictx, DIRTY);
}

int compare_str(void *src1, void *src2)
{
	return B_KEY_CMP((bkey_t *)src1, (bkey_t *)src2);
}

static int compmi(void *k1, void *k2, void * start, size_t num, int mid) {
	bkey_t  *key1 = k1;
	key_pair_t  *key2 = (key_pair_t  *) k2;

	return key_compare(key1, key2->i_key, 0, 0, 0);
}

static int comp_item(void *k1,void *k2) {
	bitem_t  *key1 = k1;
	key_pair_t  *key2 = (key_pair_t  *) k2;

	return B_ITEM_CMP(key1, key2->i_item);
}

int bp_bin_search(bkey_t *key, key_pair_t *pair, int max,
					int (*compare)(void *, void *, void * start, int num, int mid))
{
	int min = 0, mid = 0;	

	while(max >= min) {
		mid = (min + max) / 2;
				
		if (compare( (void *)key, (void *)B_KEY_PAIR(pair, mid), (void *)pair, max, mid) == 0)
			return mid;		
		else {			
			if (compare( (void *)key, (void *)B_KEY_PAIR(pair, mid), (void *)pair, max, mid) < 0)
				max = mid - 1;			
			else
				min = mid + 1;			
		}
	}

	return -1;
}

index_node_t* bp_add_root_node(master_node_t *master, index_node_t *dp, bkey_t *key,bitem_t *value) {
	index_node_t *parent_ip; 
	index_node_t *dp_left, *dp_right;
	index_node_t *node = NULL;
	key_pair_t *pair;
	key_pair_t *src, *dst;
	int i, offset = 0;

	dp_left = B_dALLOC(master, 0, ALLOC_CREATE);
	B_READ(master, dp_left, dp_left->i_offset, 0, 0);

	dp_right = B_dALLOC(master, 0, ALLOC_CREATE);
	B_READ(master, dp_right, dp_right->i_offset, 0, 0);
	
	pair = bp_alloc_pair(master->m_fanout * 2);
	if (pair == NULL) {
		printf(" alloc pair error ");
		return NULL;
	}
	
	src = dp->i_pair;	
	B_PAIR_COPY_N(pair, src, 0, 0, dp->i_num);

	bp_merge_key2(pair, key, value, dp->i_num+1);

	offset = (dp->i_num) / 2;
	dp->i_num -= offset;
	dp_left->i_num = dp->i_num;
	dp_right->i_num+= offset;
	dp_right->i_num++;

	bp_init_pair(dp_left->i_pair, master->m_fanout);
	bp_init_pair(dp_right->i_pair, master->m_fanout);

	//distribution left
	dst = dp_left->i_pair;	
	B_PAIR_COPY_N(dst, pair, 0, 0, dp_left->i_num);	
	dst = dp_right->i_pair;
	//distribution right	
	B_PAIR_COPY_N(dst, pair, 0, dp_left->i_num, dp_right->i_num);

	parent_ip = dp;
	parent_ip->i_flag = 0;
	parent_ip->i_num = 1;	
		
	bp_init_pair(parent_ip->i_pair, master->m_fanout);

	B_ITEM_COPY(B_ITEM_GET(parent_ip, 0), &dp_left->i_offset);
	B_KEY_COPY(B_KEY_GET(parent_ip, 0), B_KEY_GET(dp_left,dp_left->i_num -1));
	B_ITEM_COPY(B_ITEM_GET(parent_ip, 1), &dp_right->i_offset);
	B_KEY_COPY(B_KEY_GET(parent_ip,1), B_KEY_GET(dp_right,dp_right->i_num -1));
	
	/* insert list */
	B_NEXT(dp_right) = B_NEXT(dp_left);
	B_PREV(dp_right) = dp_left->i_offset;

	if (B_NEXT(dp_left))
	{
		node = B_dALLOC(master, B_NEXT(dp_left), ALLOC_READ);
	}

	if (node) {
		B_PREV(node) = dp_right->i_offset;
		B_WRITE(master, node, node->i_offset);
		B_RELEASE_BH(master, node->i_bh);
		B_RELEASE(master, node);
	}
	B_NEXT(dp_left) = dp_right->i_offset;

	B_WRITE(master, dp_left, dp_left->i_offset);
	B_RELEASE_BH(master, dp_left->i_bh);
	B_RELEASE(master, dp_left);

	B_WRITE(master, dp_right, dp_right->i_offset);
	B_RELEASE_BH(master, dp_right->i_bh);
	B_RELEASE(master, dp_right);

	bp_release_pair(pair);
	
	return parent_ip;
}

int bp_distribute_node(index_node_t *p_ip,key_pair_t *pair)
{
	B_PAIR_COPY_N(p_ip->i_pair, pair, 0, 0, p_ip->i_num+1);
	return 0;
}

__inline void bp_init_pair(key_pair_t *pair,int num) {
	B_PAIR_INIT_N(pair, 0, num);
}

int bp_split_data(master_node_t *master, index_node_t *p_ip,key_pair_t *child, index_node_t *sibling_ip, int count,key_pair_t *median)
{
	int i, child_count = 0;
	
	bp_init_pair(p_ip->i_pair, master->m_fanout);
	
	p_ip->i_num = 0;
	p_ip->i_root = 0;

	//distribute half 
	for (i = 0;i < count/2;i++) {		
		B_KEY_COPY(B_KEY_GET(p_ip,i), &child->i_key[child_count]);
		B_ITEM_COPY(B_ITEM_GET(p_ip, i), &child->i_item[child_count]);
		p_ip->i_num++;
		child_count++;
	}
	p_ip->i_num--;
	B_KEY_COPY(median->i_key,&child->i_key[i-1]);
	B_ITEM_COPY(median->i_item, &p_ip->i_offset);

	//distribute half 
	for (i = 0;i < count/2;i++) {
		B_KEY_COPY(B_KEY_GET(sibling_ip,i), &child->i_key[child_count]);
		B_ITEM_COPY(B_ITEM_GET(sibling_ip,i), &child->i_item[child_count]);
		sibling_ip->i_num++;
		child_count++;
	}
	sibling_ip->i_num--;
	
	return 0;
}


index_node_t* bp_split_index_node(master_node_t *master, 
									index_node_t *p_ip, 
									key_pair_t *child,
									int count,
									key_pair_t *median,
									int is_leaf) 
{
	index_node_t *sibling_ip;	

	if (count-1 < master->m_fanout) {
		bp_distribute_node(p_ip, child);
		return NULL;
	}

	sibling_ip = B_iALLOC(master, 0, ALLOC_CREATE);
	B_READ(master, sibling_ip, sibling_ip->i_offset, 0, 0);
	bp_split_data(master ,p_ip, child, sibling_ip, count, median);

	return sibling_ip;	
}

int bp_split_data_node(master_node_t *master,
						index_node_t *ip,
						index_node_t *dp,
						bkey_t *key,
						bitem_t *value,
						key_pair_t *pair)
{
	index_node_t *dp_left = dp, *dp_right;		
	index_node_t *node = NULL;
	key_pair_t *pair_array;
	int i, offset = 0, count = 0;
	int alloc_num = dp->i_num + 1;	
	int seq_detection = 1;
	pair_array = bp_alloc_pair(alloc_num);
	if (pair_array == NULL)
		return 0;

	dp_right = B_dALLOC(master, 0, ALLOC_CREATE);
	B_READ(master, dp_right, dp_right->i_offset, 0, 0);

	for (i = 0;i < ip->i_num+1;i++) {
		if (!B_ITEM_CMP(B_ITEM_GET(ip, i), &dp->i_offset)) {
				B_KEY_INIT(B_KEY_GET(ip, i));
				B_ITEM_INIT(B_ITEM_GET(ip, i));
				break;	
		}
	}

#if 0
	for (i = 0; i < dp->i_num - 1; i++) {
		u64 key1 = *B_KEY_GET(dp, i);
		u64 key2 = *B_KEY_GET(dp, (i + 1));
		
		if (key1 != key2 - 1) {
			seq_detection = 0;
			break;
		}
		seq_detection = 1;
	}

	if (seq_detection)
	{
	//	printf(" Need to optimize that data node contains consecutive keys \n");
	}
#endif

	B_PAIR_COPY_N(pair_array, dp->i_pair, 0, 0, dp->i_num);
	bp_merge_key2(pair_array, key, value, dp->i_num+1);

	offset = (dp->i_num) / 2;	
	dp->i_num -= offset;
	dp_right->i_num+= offset;
	dp_right->i_num++;

	//init 
	bp_init_pair(dp_left->i_pair, master->m_fanout);
	bp_init_pair(dp_right->i_pair, master->m_fanout);

	//distribute 
	B_PAIR_COPY_N(dp_left->i_pair, pair_array, 0, 0,dp->i_num);
	B_PAIR_COPY_N(dp_right->i_pair, pair_array, 0, dp->i_num,dp_right->i_num);
	
	/* insert list */
	B_NEXT(dp_right) = B_NEXT(dp_left);
	B_PREV(dp_right) = dp_left->i_offset;

	if (B_NEXT(dp_left)) {
		node = B_dALLOC(master, B_NEXT(dp_left), ALLOC_READ);
		B_READ(master, node, node->i_offset, 1, 0);
	}
	if (node) {
		if (!B_NEXT(node) && !B_PREV(node))
			node = node;
		B_PREV(node) = dp_right->i_offset;
		B_WRITE(master, node, node->i_offset);
		B_RELEASE_BH(master, node->i_bh);
		B_RELEASE(master, node);
	}
	B_NEXT(dp_left) = dp_right->i_offset;

	if (!B_NEXT(dp_left) && !B_PREV(dp_left))
		dp_left = dp_left;
	
	if (!B_NEXT(dp_right) && !B_PREV(dp_right))
		dp_right = dp_right;

	count = 0;	
	for (i = 0;i < ip->i_num+1;i++) {		
		if (!B_KEY_ISNULL(B_KEY_GET(ip,i))) {
			B_KEY_COPY(&pair->i_key[count], B_KEY_GET(ip,i));
			B_ITEM_COPY(&pair->i_item[count], B_ITEM_GET(ip,i));
			count++;
		}
	}
	
	bp_merge_key2(pair, B_KEY_GET(dp_left, dp_left->i_num-1),&dp_left->i_offset,++count);
	bp_merge_key2(pair, B_KEY_GET(dp_right, dp_right->i_num-1),&dp_right->i_offset,++count);

	B_WRITE(master, dp_left, dp_left->i_offset);
	B_WRITE(master, dp_right,dp_right->i_offset);
	B_RELEASE_BH(master, dp_left->i_bh);
	B_RELEASE_BH(master, dp_right->i_bh);
	B_RELEASE(master, dp_left);
	B_RELEASE(master, dp_right);

	bp_release_pair(pair_array);

	return 1;
}


int bp_split_tree(master_node_t *master, index_node_t *dp, bkey_t *key, bitem_t *value) {	
	index_node_t *new_child = NULL;
	index_node_t *ip = NULL;
	key_pair_t *pair_arr = NULL;	
	key_pair_t *median = NULL;	
	int count = 0; 
	int i;			
	int ip_offset = 0;	
		
	median = bp_alloc_pair(1);
	ip_offset = B_POP(master);
	ip = B_iALLOC(master, ip_offset, ALLOC_READ);
	B_READ(master, ip, ip->i_offset, 1, 0);
	pair_arr = bp_alloc_pair(master->m_fanout+1);
	if (pair_arr == NULL) {
		printf(" alloc pair error ");
		return 0;
	}

	while(1) {
		bp_init_pair(pair_arr, master->m_fanout+1);		
		if (dp) {
			bp_split_data_node(master, ip , dp, key, value, pair_arr);
			dp = NULL;			
			ip->i_num++; 
			count = ip->i_num +1;
			new_child = bp_split_index_node(master, ip, pair_arr, count, median, 1);
		} else if (new_child) {				
			bkey_t *k;
			int ptr = 0;

			k = B_KEY_ALLOC();
			if (k == NULL) {
				printf(" malloc error \n");
				return -1;
			}
			memset(k, 0x00, BP_KEY_SIZE);

			ip->i_num++;
			count = ip->i_num+1;
			
			for (i =0;i < ip->i_num;i++) {				
				if (!B_ITEM_CMP(B_ITEM_GET(ip,i), median->i_item)) {
					B_KEY_COPY(k,B_KEY_GET(ip,i));					
				} else {					
					B_PAIR_COPY(pair_arr,ip->i_pair, ptr,i);
					ptr++;
				}		
			}

			bp_merge_key2(pair_arr, median->i_key, median->i_item, ip->i_num);
			B_PAIR_INIT(median, 0);								
						
			if (B_KEY_CMP(k,B_KEY_GET(new_child,new_child->i_num)) > 0)
			{
				bp_merge_key2(pair_arr, k, &new_child->i_offset, ip->i_num+1);
			} else {
				bp_merge_key2(pair_arr, B_KEY_GET(new_child,new_child->i_num), &new_child->i_offset, ip->i_num+1);
			}
						
			bubble_sort(pair_arr, count, compare_str);

			B_WRITE(master, new_child, new_child->i_offset);
			B_RELEASE_BH(master, new_child->i_bh);
			B_RELEASE(master, new_child);

			new_child = bp_split_index_node(master, ip, pair_arr, count, median, 0);			
			if(new_child)
				new_child = new_child;
			B_KEY_FREE(k);
		}
		
		B_WRITE(master, ip, ip->i_offset);
		if (new_child == NULL)
			break;		
		if (ip->i_root)
			break;
		if (!master->m_sp)
			break;

		B_RELEASE_BH(master, ip->i_bh);
		ip_offset = B_POP(master);
		B_READ(master, ip, ip_offset, 1, 0);
	}
	
	if (new_child != NULL) { //split root node
		index_node_t *temp = ip, *root;

		root = B_iALLOC(master, 0, ALLOC_CREATE);			
		B_READ(master, root, root->i_offset, 0, 0);

		if (B_KEY_CMP(B_KEY_GET(ip,ip->i_num-1),B_KEY_GET(new_child,new_child->i_num-1)) < 0) {
			B_ITEM_COPY(B_ITEM_GET(root,0), &ip->i_offset);
			B_ITEM_COPY(B_ITEM_GET(root,1), &new_child->i_offset);			
			
			B_KEY_COPY(B_KEY_GET(root,0), median->i_key);
			B_KEY_COPY(B_KEY_GET(root,1), B_KEY_GET(new_child,new_child->i_num-1));
		} else {
			B_ITEM_COPY(B_ITEM_GET(root,0), &new_child->i_offset);
			B_ITEM_COPY(B_ITEM_GET(root,1), &ip->i_offset);

			B_KEY_COPY(B_KEY_GET(root,0), median->i_key);		
			B_KEY_COPY(B_KEY_GET(root,1), B_KEY_GET(ip,ip->i_num-1));
		}

		temp->i_root = 0;
		new_child->i_root = 0;		
		root->i_root = 1;		

		master->m_root = root->i_offset;

		bp_write_master(master);

		root->i_num = 1;

		B_WRITE(master, root, root->i_offset);
		B_WRITE(master, ip, ip->i_offset);
		B_WRITE(master, new_child, new_child->i_offset);
		B_RELEASE_BH(master, root->i_bh);
		B_RELEASE_BH(master, new_child->i_bh);
		B_RELEASE(master, root);		
		B_RELEASE(master, new_child);
	}

	B_RELEASE_BH(master, ip->i_bh);
	B_RELEASE(master, ip);
		
	bp_release_pair(median);
	bp_release_pair(pair_arr);
	return 0;
}

void bp_insert_value_tree(index_node_t *ip,
							int index,
							bkey_t *key,
							bitem_t *value)
{
	if (!B_KEY_CMP(B_KEY_PAIR(ip->i_pair, index), key))		
		B_ITEM_COPY(B_ITEM_PAIR(ip->i_pair, index), value);
	else
		printf(" Warning: key is mismatched\n");	
}

int bp_find_key(master_node_t *master, bkey_t *key, bitem_t *value) {
	int index;
	int res;
	bitem_t temp;

	pthread_mutex_lock(&master->m_big_lock);
		
	index = B_SEARCH(master, key, NULL);
	if (index>= 0) {
		B_ITEM_COPY(value, B_ITEM_PAIR(master->m_cur->i_pair, index));	
	} else
		*value = 0;

	B_FLUSH_STACK(master);
	B_RELEASE_BH(master, master->m_cur->i_bh);
	B_RELEASE(master, master->m_cur);	
	master->m_cur = 0;
	
	pthread_mutex_unlock(&master->m_big_lock);

	return index;
}

int bp_update_key_tree(master_node_t *master, bkey_t *key, bitem_t *value) 
{
	int index;
	int res;
	res = pthread_mutex_lock(&master->m_big_lock);
	if (res)
		printf(" pthread mutex lock error  = %d\n", res);

	index = B_SEARCH(master, key, NULL);
	if (index < 0) {
		printf("not found \n");		
		goto RES;
	}
	
	bp_insert_value_tree(master->m_cur,index, key, value);
	B_WRITE(master, master->m_cur, master->m_cur->i_offset);
	B_FLUSH_STACK(master);	
	B_RELEASE_BH(master, master->m_cur->i_bh);
	B_RELEASE(master, master->m_cur);	
	master->m_cur = 0;
	
RES:;

	res = pthread_mutex_unlock(&master->m_big_lock);
	if (res)
		printf(" pthread mutex unlock error  = %d\n", res);
	return 0;

}

int bp_key_is_null(bkey_t *buf)
{
	int i;

	for (i = 0;i < INDEX_KEY_LEN;i++) {
		if (buf[i])
			return 0;
	}

	return 1;
}

int bp_merge_key(master_node_t *master, index_node_t *dp, bkey_t *key,bitem_t *value)
{
	int i = 0, j = 0;
	int max = master->m_fanout;
	key_pair_t *pair = dp->i_pair;
	
	max = dp->i_num+1;
	for (i = max-2; i >=0; i--)
	{
		if (!B_KEY_ISNULL(B_KEY_PAIR(pair,i)) && B_KEY_CMP(B_KEY_PAIR(pair,i), key) < 0) 
		{
			break;		
		}		
	}
	
	i++;
	for (j = max-1;j > i;j--) 
	{
		B_PAIR_COPY(pair,pair,j,j-1);				
		B_PAIR_INIT(pair, j-1);					
	}			

	if (B_KEY_ISNULL(B_KEY_PAIR(pair,i)))
	{
		B_KEY_COPY(B_KEY_PAIR(pair,i), key);
		B_ITEM_COPY(B_ITEM_PAIR(pair,i), value);	
		dp->i_num++;
	}

	return 0;
}

int bp_insert_key_tree(master_node_t *master,
									bkey_t *key, 
									bitem_t *value,
									bitem_t *cur_value,
									int update)
{
	int i, j, index, res = 0;
	index_node_t *root;
	index_node_t *dp;
	key_pair_t *pair;
	
	res = pthread_mutex_lock(&master->m_big_lock);
	if (res)
		printf(" pthread mutex lock error  = %d\n", res);

	/* root node allocation */
	root = B_dALLOC(master, master->m_root, ALLOC_READ);
	/* read root node from disk */
	B_READ(master, root, root->i_offset, HEAD_SYNC, NOLOCK);
	/* keep current node pointer temporalily to quickly retrieve */
	master->m_cur = root;
		
	dp = traverse_empty(master, root, key);
	index = B_GET_PAIR(master, dp, key);
	if (index >=0)
	{
		if (cur_value != NULL)
			B_ITEM_COPY(cur_value, B_ITEM_PAIR(master->m_cur->i_pair, index));

		res = -1;
		if (update && !B_KEY_CMP(B_KEY_PAIR(dp->i_pair, index), key)) 
		{
			B_ITEM_COPY(B_ITEM_PAIR(dp->i_pair, index), value);			
			B_WRITE(master, dp, dp->i_offset);			
			res = 0;
			//bp_insert_lookup_cache(*key, *value);
		}

		B_RELEASE_BH(master, dp->i_bh);
		B_FLUSH_STACK(master);	
		B_RELEASE(master, master->m_cur);	
		master->m_cur = 0;

		//return res;
		goto RES;
	}

	//check overflow in case of root node
	if (dp->i_root && dp->i_num == master->m_fanout) 
	{
		root = bp_add_root_node(master, dp, key, value);
		root->i_root = 1;
		B_WRITE(master, root, root->i_offset);
		B_RELEASE_BH(master, root->i_bh);
		B_RELEASE(master, root);
	} 
	else if (dp->i_num == master->m_fanout) 
	{
		bp_split_tree(master, dp, key, value);
	} 
	else	
	{
		bp_merge_key(master, dp, key, value);
		B_WRITE(master, dp, dp->i_offset);
		B_RELEASE_BH(master, dp->i_bh);
		B_RELEASE(master, dp);
	}

	master->m_cur = 0;
	B_FLUSH_STACK(master);		
	master->m_key_count++;

RES:;
	
	pthread_mutex_unlock(&master->m_big_lock);

	return res;	
}


int bp_redist_data_child(master_node_t *master,index_node_t *ip,index_node_t *child, int data_node) {
	index_node_t *child2;
	index_node_t *node, *de_alloc = NULL;
	index_node_t *prev = NULL, *next = NULL;
	key_pair_t *pair;
	int count = 0, alloc_num = 0;
	int i,key_count = 0;		
	int target1 = 0, target2 = 0;	
	int max_count;

	for (i = 0;i < ip->i_num + 1;i++) {
		if (!B_ITEM_CMP(B_ITEM_GET(ip,i), &child->i_offset)) {
			target1 = i;
			break;
		}
	}
	
	if (target1 == 0)
		target2 = 1;
	else if (target1 >= ip->i_num)	
		target2 = ip->i_num-1;	
	else
		target2 = target1 + 1;
	
	pair = bp_alloc_pair(master->m_fanout * 2);
	if (pair == NULL) {
		printf(" bp alloc error \n");
		exit(1);
		return 0;
	}

	if (data_node) {
		//collect keys and items  
		B_PAIR_COPY_N(pair, child->i_pair, 0, 0, child->i_num);

		child2 = B_dALLOC(master, *B_ITEM_GET(ip, target2), ALLOC_READ);
		B_READ(master, child2, child2->i_offset, 1, 0);

		B_PAIR_COPY_N(pair, child2->i_pair, child->i_num, 0, child2->i_num);
		count =child->i_num + child2->i_num;

	} else {
		//collect keys and items  
		B_PAIR_COPY_N(pair, child->i_pair, 0, 0, child->i_num+1);
		
		child2 = B_dALLOC(master, *B_ITEM_GET(ip, target2), ALLOC_READ);
		B_READ(master, child2, child2->i_offset, 1, 0);
		
		B_PAIR_COPY_N(pair, child2->i_pair, child->i_num+1, 0, child2->i_num+1);
		count = child->i_num +1 + child2->i_num + 1;
	}
	
	bubble_sort(pair, count, compare_str);

	if (data_node)
		max_count = master->m_fanout;
	else
		max_count = master->m_fanout + 1;

	if (count < max_count) //merge child1 and child2 
	{
		// delete key and item in parent node 	
		i = target1;
		node = child2;
		de_alloc = child;
		if (target2 < target1) 
		{
			i = target2;
			node = child;
			de_alloc = child2;
		}

		for (; i < ip->i_num; i++) 
		{
			B_PAIR_COPY(ip->i_pair, ip->i_pair, i, i+1);
		}		
		B_PAIR_INIT(ip->i_pair, i);

		ip->i_num--;

		node->i_num = count;
		count = 0;		
		for (i = 0; i < master->m_fanout; i++)
		{
			if (i < node->i_num) {				
				B_PAIR_COPY(node->i_pair, pair, i, count);
				count++;
			}
		}	
		
		// remove dealloc node from double list
		if (data_node) 
		{
			if (B_PREV(de_alloc) == node->i_offset)
				prev = node;
			else if (B_NEXT(de_alloc) == node->i_offset)
				next = node; 

			if (B_PREV(de_alloc) == 0 && B_NEXT(de_alloc) == 0)
				de_alloc = de_alloc;
			if (prev == NULL && B_PREV(de_alloc)) {
				prev = B_dALLOC(master, B_PREV(de_alloc), ALLOC_READ);
				B_READ(master, prev, prev->i_offset, 1, 0);
			}

			if (next == NULL && B_NEXT(de_alloc)) {
				next = B_dALLOC(master, B_NEXT(de_alloc), ALLOC_READ);
				B_READ(master, next, next->i_offset, 1, 0);
			}

			if (prev)
				B_NEXT(prev)= B_NEXT(de_alloc);
			
			if (next)
				B_PREV(next)= B_PREV(de_alloc);
			
			if (prev != node && prev) {
				B_WRITE(master, prev, prev->i_offset);
				B_RELEASE_BH(master, prev->i_bh);
				B_RELEASE(master, prev);
			}
			
			if (next != node && next) {				
				B_WRITE(master, next, next->i_offset);
				B_RELEASE_BH(master, next->i_bh);
				B_RELEASE(master, next);
			}
		}

		if (!data_node)
			node->i_num--;

		B_DEALLOC(master, de_alloc);
	} 
	else
	{
		if (target2 < target1) {			
			index_node_t *t;
			t = child2;
			child2 = child;
			child = t;
		}

		child->i_num = count/2;
		child2->i_num = count/2;
		if (count % 2)
		{
			child2->i_num++;
		}

		//distribute 
		count = 0;
		B_PAIR_COPY_N(child->i_pair, pair, 0, 0, child->i_num);		
		B_PAIR_INIT_N(child->i_pair, child->i_num, (master->m_fanout - child->i_num));

		B_PAIR_COPY_N(child2->i_pair, pair, 0, child->i_num, child2->i_num);		
		B_PAIR_INIT_N(child2->i_pair, child2->i_num, (master->m_fanout - child2->i_num));
		
		//change key
		for (i = 0;i < ip->i_num + 1;i++) {	
			if (!B_ITEM_CMP(B_ITEM_GET(ip,i), &child->i_offset)) {
				B_KEY_COPY(B_KEY_GET(ip,i), B_KEY_GET(child, child->i_num-1));				
			}		
			if (!B_ITEM_CMP(B_ITEM_GET(ip,i), &child2->i_offset)) {
				if (B_KEY_CMP(B_KEY_GET(child2, child2->i_num-1),B_KEY_GET(ip,i)) > 0) {
					B_KEY_COPY(B_KEY_GET(ip,i), B_KEY_GET(child2, child2->i_num-1));
				}
			}
		}		

		if (!data_node) {
			child->i_num--;
			child2->i_num--;
		}
	}

	bp_release_pair(pair);

	if (de_alloc)
	{
		de_alloc->i_status = INDEX_NODE_FREE;		
		memset(de_alloc->i_bh->bh_buf, 0x00, CLUSTER_SIZE);
		B_WRITE(master, de_alloc, de_alloc->i_offset);
	}

	if (de_alloc != child)
		B_WRITE(master, child, child->i_offset);
	if (de_alloc != child2)
		B_WRITE(master, child2, child2->i_offset);
	
	B_WRITE(master, ip, ip->i_offset);

	B_RELEASE_BH(master, child->i_bh);
	B_RELEASE_BH(master, child2->i_bh);
	B_RELEASE(master, child2);
	B_RELEASE(master, child);	

	if (ip->i_num+1 >= (master->m_fanout/2+1))//INDEX_MIN_WAY
		return 0; 
	else 		
		return 1; //under flow
}


int bp_merge_key_tree(master_node_t *master,bkey_t *key, index_node_t *dp, int d_index) {
	index_node_t *index_temp = NULL;	
	index_node_t *ip = dp;
	index_node_t *root = NULL;
	int ret = 1;
	int ip_offset;

	if (B_ISLEAF(ip) && B_ISROOT(ip)) 
	{
		B_WRITE(master, ip, ip->i_offset);
		B_RELEASE_BH(master, ip->i_bh);
		B_RELEASE(master, ip);
		return 0;
	}
	
	while(1) 
	{
		ip_offset = B_POP(master);
		ip = B_iALLOC(master, ip_offset, ALLOC_READ);
		B_READ(master, ip, ip->i_offset, 1, 0);

		if (ret) {		
			if (B_ISLEAF(dp))
				ret = bp_redist_data_child(master, ip, dp, 1);
			else
				ret = bp_redist_data_child(master, ip, index_temp, 0);
				
			if (ret)//underflow
				index_temp = ip;				
			else
				index_temp = NULL;				

			B_WRITE(master, ip, ip->i_offset);
		}
		
		if (B_ISROOT(ip))
			break;
		if (!ret)
			break;		
	}

	if (ip->i_root && ip->i_num == 0) 
	{
		B_DEALLOC(master, ip);
			
		master->m_root = *B_ITEM_GET(ip,0);
		bp_write_master(master);

		root = B_iALLOC(master, master->m_root, ALLOC_READ);
		B_READ(master, root, root->i_offset, 1, ALLOC_READ);
		root->i_root = 1;
		B_WRITE(master, root, root->i_offset);

		/* necessary to mark as a free node*/
		B_WRITE(master, ip, ip->i_offset);

		B_RELEASE_BH(master, ip->i_bh);
		B_RELEASE_BH(master, root->i_bh);
		B_RELEASE(master, root);
		B_RELEASE(master, ip);
		ip = NULL;
	}

	if (ip) 
	{
		B_WRITE(master, ip, ip->i_offset);
		B_RELEASE_BH(master, ip->i_bh);
		B_RELEASE(master, ip);
	}
	
	return ret;
}

static int bp_compare_index_node(void *k1, void *k2, void *start, int num, int mid) {
	bkey_t  *key1 = (bkey_t *) k1;
	bkey_t *key2 = (bkey_t *) k2;	
	key_pair_t	*base = (key_pair_t  *) start;
	key_pair_t *prev_key;
	int ret1, ret2;
	int last;
	int prev;
		
	last = mid - 0;	
	prev = last-1;
	
	if (prev < 0) 
	{
		prev_key = NULL;
		ret1 = 1;
	} else {
		ret1 = B_KEY_CMP(key1, &base->i_key[prev]);
	}
	
	ret2 = B_KEY_CMP(key1, key2);

	if (ret1 >0 && ret2 <= 0) { 
		ret2 = 0;
		return ret2;
	}
		
	return ret2;
}

index_node_t *bp_next_node(master_node_t *master, index_node_t *ip, bkey_t *key)
{
	key_pair_t *pair = NULL;
	int offset = 0;
	int key_num = 0;
	if (B_KEY_CMP(key, B_KEY_GET(ip,ip->i_num-1)) > 0) {		
		offset = *B_ITEM_GET(ip,ip->i_num);
	} else {
		key_num = bp_bin_search(key, ip->i_pair, ip->i_num, bp_compare_index_node);
		offset = ip->i_pair->i_item[key_num];		
	}

	B_RELEASE_BH(master, ip->i_bh);

	B_READ(master, ip, offset, 1, 0);
	return ip;
}

index_node_t* traverse_empty(master_node_t *master, index_node_t *ip, bkey_t *key)
{
	while(!B_ISLEAF(ip))
	{		
		B_PUSH(master, ip->i_offset);
		ip = bp_next_node(master, ip, key);		

		if (ip->i_offset == 0)
			printf(" debug");

		assert(ip->i_offset);		
	}

	return (index_node_t *)ip;
}

int rsearch_data_node(master_node_t *master, bkey_t *s_key, bkey_t *e_key)
{
	index_node_t *ip;	
	bitem_t item = 0;
	int index;
	
	ip = B_iALLOC(master, master->m_root, ALLOC_READ);
	B_READ(master, ip, ip->i_offset, 1, 1);

	while (!B_ISLEAF(ip)) 
	{
		B_PUSH(master, ip->i_offset);
		ip = bp_next_node(master, ip, s_key);
	}

	if (B_ISLEAF(ip))
	{
		master->m_cur = ip;
		index = B_GET_PAIR(master, ip, s_key);	
	}	

	if (index < 0)
		index = 0;
	
	while(1)
	{
		if (B_KEY_CMP(s_key, B_KEY_GET(ip, index)) <= 0)
			break;
		index++;
	}
		
	while(1)
	{
		//printf(" key item : %d,%d\n", *B_KEY_GET(ip, index), *B_ITEM_GET(ip, index));
		index++;
		if (index == ip->i_num)
		{
			B_READ(master, ip, B_NEXT(ip), 1, 1);
			index = 0;
		}

		if (B_KEY_CMP(e_key, B_KEY_GET(ip, index)) < 0)
			break;
	}
	
	B_FLUSH_STACK(master);	
	B_RELEASE(master, master->m_cur);	
	master->m_cur = 0;
	
	return 0;
}

int search_data_node(master_node_t *master, bkey_t *key, index_node_t **d)
{
	index_node_t *ip;

	ip = B_iALLOC(master, master->m_root, ALLOC_READ);
	B_READ(master, ip, ip->i_offset, 1, READ_LOCK);
	
	while (!B_ISLEAF(ip))
	{
		B_PUSH(master, ip->i_offset);
		ip = bp_next_node(master, ip, key);
	}
			
	if (B_ISLEAF(ip)) 
	{
		if (d)
			*d = ip;		 
		master->m_cur = ip;
		return B_GET_PAIR(master, ip, key);	
	}	

	return -1;
}

int get_pair_tree(index_node_t *dp, bkey_t *key)
{
	int key_num;
	key_num = bp_bin_search(key, dp->i_pair, dp->i_num-1, key_compare);
	
	if (key_num < 0)
		return key_num;
	else 
		return key_num;

}

int bp_remove_key(master_node_t *master, bkey_t *key) {
	index_node_t *dp;
	key_pair_t *pair,*base;
	int  i;
	int res = 0;

	res = pthread_mutex_lock(&master->m_big_lock);
	if (res)
		printf(" pthread mutex lock error  = %d\n", res);

	i = B_SEARCH(master, key, &dp);
	if (i < 0) 
	{		
		printf("not found [%lu] in remove key\n", (long)*key);
		res = -1;
		goto RES; 
	}

	if (B_KEY_CMP(B_KEY_GET(dp, i), key))
	{
		printf(" invalid key = %lu\n", (long)*key);
		res = -1;
		goto RES;
	}	
	
	for (; i < dp->i_num-1; i++)
	{
		B_PAIR_COPY(dp->i_pair,dp->i_pair, i, i+1);
	}
	B_PAIR_INIT(dp->i_pair, i);
	dp->i_num--;

	if (dp->i_num >= (master->m_fanout / 2 + 1))
	{//INDEX_MIN_WAY
		B_WRITE(master, master->m_cur, master->m_cur->i_offset);
		if (dp != master->m_cur)
			B_RELEASE(master, dp);		
		B_RELEASE_BH(master, master->m_cur->i_bh);
		B_RELEASE(master, master->m_cur);
		master->m_cur = 0;	
	}
	else
	{
		bp_merge_key_tree(master, key, dp, i);
	}
		
	B_FLUSH_STACK(master);
		
	master->m_key_count--;

RES:;
	res= pthread_mutex_unlock(&master->m_big_lock);
	if (res)
		printf(" pthread mutex unlock error  = %d\n", res);

	return 1;	
}


void bp_copy_node_to_raw(index_node_t *node, char *raw)
{
	if (node->i_flag != 0 && node->i_flag != 1)
	{
		printf(" error copy node to raw \n");
	}

	memcpy(raw, node, BP_NODE_HEAD_SIZE);
	raw += BP_NODE_HEAD_SIZE;
}

void bp_print_node(index_node_t *node)
{
	printf(" node lbno = %d \n", node->i_offset);
	printf(" bh load = %d \n", node->i_bh->bh_bc->bc_load);
	printf(" bh pno = %d\n", node->i_bh->bh_bc->bc_pno);
	printf(" bh ino = %d\n", node->i_bh->bh_bc->bc_ino);
	printf(" bh data = %s\n", node->i_bh->bh_bc->bc_buf);
}


void bp_copy_raw_to_node(index_node_t *node, char *raw)
{	
	memcpy(node, raw, BP_NODE_HEAD_SIZE);

	if (node->i_flag != 0 && node->i_flag != 1)
	{
		bp_print_node(node);
		assert(0);
	}

	if (node->i_offset == 0)
	{
		assert(0);
	}
}

struct nvfuse_buffer_head *bp_read_block(master_node_t *master, int offset, int rwlock)
{	
	struct nvfuse_superblock *sb = master->m_sb;	
	struct nvfuse_buffer_head *bh = NULL;
	u32 bno;	
	s32 res;
	lbno_t p_offset;
	
	p_offset = offset/BP_CLUSTER_PER_NODE;
	bh = nvfuse_get_bh(sb, master->m_ictx, master->m_ino, p_offset, READ, NVFUSE_TYPE_META);
	
	return bh;
}

int bp_write_block(struct nvfuse_superblock *sb, struct nvfuse_buffer_head *bh, char *buf, int offset)
{
	nvfuse_release_bh(sb, bh, 0, DIRTY);
	return 0;
}

int bp_read_node(master_node_t *master, index_node_t *node, int offset, int sync, int rwlock)
{		
	node->i_bh = bp_read_block(master, offset, rwlock);
	node->i_buf = node->i_bh->bh_buf+ BP_NODE_SIZE*(offset%BP_CLUSTER_PER_NODE);

	node->i_pair->i_key = (bkey_t *)(node->i_buf + BP_KEY_START);
	node->i_pair->i_item = (bitem_t *)(node->i_buf + BP_ITEM_START(master));

	if (sync)
		bp_copy_raw_to_node(node, node->i_buf);
		
	if (node->i_flag != 0 && node->i_flag != 1) 
	{
		printf(" Error: Invalid or Corrupted node data \n");
		bp_print_node(node);		
		assert(0);
	}

	return 0; 
}

int bp_read_master(master_node_t *master)
{
	int offset = 0;
	struct nvfuse_buffer_head *bh;
	
	master->m_ictx = nvfuse_read_inode(master->m_sb, NULL, master->m_ino);

	bh = bp_read_block(master, offset, READ_LOCK);
	master->m_buf = bh->bh_buf;
	master->m_bh = bh;

	memcpy(master, bh->bh_buf, BP_NODE_HEAD_SIZE);
		
	B_RELEASE_BH(master, bh);

	return 0;
}

void bp_write_master(master_node_t *master)
{
	struct nvfuse_buffer_head *bh;
	char *raw = NULL;
	int offset = 0;

	bh = bp_read_block(master, offset, WRITE_LOCK);
	master->m_buf = bh->bh_buf;
	master->m_bh = bh;

	raw = bh->bh_buf;

	memcpy(raw, master, BP_NODE_HEAD_SIZE);
	
	nvfuse_mark_dirty_bh(master->m_sb, bh);
	B_RELEASE_BH(master, bh);
}

int bp_write_node(master_node_t *master, index_node_t *node, int offset)
{
	assert(node->i_bh);

	bp_copy_node_to_raw(node, node->i_buf);	
	nvfuse_mark_dirty_bh(master->m_sb, node->i_bh);

	return 1;
}
/* necessary a bitmap table to quickly find a free node */
/* alloc free block in a b+tree file*/
offset_t bp_alloc_bitmap(master_node_t *master, struct nvfuse_inode_ctx *ictx)
{	
	struct nvfuse_buffer_head *bh;	
	struct nvfuse_inode *inode;
	index_node_t *node;
	u32 new_bno = 0;

	
	if (master->m_dealloc_block) 
	{
		u32 cnt = 0;
		u32 lblock = master->m_bitmap_ptr;
		
		//ictx = nvfuse_read_inode(master->m_sb, NULL, master->m_ino);
		inode = ictx->ictx_inode;

		while (cnt++ < (inode->i_size >> CLUSTER_SIZE_BITS))
		{
			bh = nvfuse_get_bh(master->m_sb, master->m_ictx, master->m_ino, lblock, READ, NVFUSE_TYPE_META);
			if (!bh)
			{
				printf(" Warning: read block error \n");
				return 0;
			}
			node = (index_node_t *)bh->bh_buf;
			if (node->i_status == INDEX_NODE_FREE)
			{				
				new_bno = lblock;
				//printf(" new bno = %d\n ", new_bno);

				master->m_dealloc_block--;
				memset(bh->bh_buf, 0x00, CLUSTER_SIZE);
				node->i_status = INDEX_NODE_USED;
				master->m_bitmap_ptr = lblock;
			}
			
			if (new_bno)
				nvfuse_release_bh(master->m_sb, bh, INSERT_HEAD, DIRTY);
			else
				nvfuse_release_bh(master->m_sb, bh, INSERT_TAIL, CLEAN);

			if (new_bno)
				break;

			lblock = (lblock + 1) % (inode->i_size >> CLUSTER_SIZE_BITS);
		}
	}

	if (!new_bno)	
	{
		s32 ret;
		inode = ictx->ictx_inode;

		new_bno = inode->i_size >> CLUSTER_SIZE_BITS;
		ret = nvfuse_get_block(master->m_sb, ictx, new_bno, 1/* num block */, NULL, NULL, 1);
		if (ret)
		{
			printf(" data block allocation fails.");
			return NVFUSE_ERROR;
		}
		bh = nvfuse_get_new_bh(master->m_sb, ictx, inode->i_ino, new_bno, NVFUSE_TYPE_META);
		
		node = (index_node_t *)bh->bh_buf;
		node->i_status = INDEX_NODE_USED;

		nvfuse_release_bh(master->m_sb, bh, INSERT_HEAD, DIRTY);
		assert(inode->i_size < MAX_FILE_SIZE);
		inode->i_size += CLUSTER_SIZE;		
		nvfuse_mark_inode_dirty(ictx);		
	}
		
	master->m_alloc_block++;	
	bp_write_master(master);	
	
	assert(new_bno);

	return new_bno;
}

/* deallocation disk space */
int bp_dealloc_bitmap(master_node_t *master, index_node_t *p)
{
	p->i_status = INDEX_NODE_FREE;
	master->m_bitmap_ptr = p->i_offset;
	master->m_dealloc_block++;
		
	bp_write_master(master);

	return 0;
}

index_node_t *bp_alloc_node(master_node_t *master, int flag, int offset, int is_new)
{
	index_node_t *node = NULL;		
	
	node = (index_node_t *)bp_malloc(sizeof(index_node_t));
	if (node == NULL)
	{
		printf(" cannot allocate memory \n");
		return NULL;
	}	
	master->m_size += sizeof(index_node_t);
	memset(node, 0x00, sizeof(index_node_t));
	node->i_flag = flag;
	node->i_status = INDEX_NODE_USED;

	if (offset)
	{
		node->i_offset = offset;
	}

	if (is_new)
	{
		node->i_offset = bp_alloc_bitmap(master, master->m_ictx);
		master->m_fsize += master->m_node_size;
	}

	node->i_pair = (key_pair_t *)bp_malloc(sizeof(key_pair_t));
	if (node->i_pair == NULL) {
		printf(" malloc error \n");
		return NULL;
	}
	master->m_size += sizeof(key_pair_t);
	node->i_master = master;

	return node;
}

int bp_release_bh(struct nvfuse_buffer_head *bh) {
	if (bh == NULL)
		return 0;
		
	nvfuse_release_bh(bh->bh_bc->bc_sb, bh, INSERT_HEAD, bh->bh_bc->bc_dirty);

	return 0;
}

int bp_release_node(master_node_t *master, index_node_t *p) {
	
	if (!p)
		return 0;

	p->i_pair->i_item = NULL;
	p->i_pair->i_key = NULL;

	bp_free((void *)(p->i_pair));

	p->i_pair = NULL;
	
	master->m_size -= sizeof(key_pair_t);
	bp_free(p);
	p = NULL;

	master->m_size -= sizeof(index_node_t);	
	
	return 0;
}

void stack_push(master_node_t *master, offset_t v)
{
	int i;

	for (i = 0; i < master->m_sp; i++) {
		if (master->m_stack[i] == v)
			printf(" stack debug \n");
	}
	master->m_stack[master->m_sp++] = v;
	if (master->m_sp >= MAX_STACK)
	{
		printf(" b+tree stack overflow \n");
		assert(0);
	}
}

offset_t stack_pop(master_node_t *master)
{
	offset_t temp;
	master->m_sp--;
	
	if (master->m_sp < 0)
	{
		printf(" stack underflow ");
		assert(0);
	}

	temp = master->m_stack[master->m_sp];
	master->m_stack[master->m_sp] = 0;
	return temp;
}

#ifdef KEY_IS_INTEGER
__inline int key_compare(void *k1, void *k2, void *start, int num, int mid)
{
	if (*(bkey_t *)k1 > *(bkey_t *)k2)
		return 1;
	else if (*(bkey_t *)k1 == *(bkey_t *)k2)
		return 0;
	else
		return -1;
}
#else
__inline int key_compare(void *k1, void *k2, void *start, int num, int mid)
{
	int ret = B_KEY_CMP(k1, k2);

	if (ret > 0)
		return 1;
	else if (ret == 0)
		return 0;
	else
		return -1;
}
#endif
int bp_merge_key2(key_pair_t *pair, bkey_t *key, bitem_t *value, int max) {
	int i = 0, j = 0;

	for (i = max - 2; i >= 0; i--) 
	{
		if (!B_KEY_ISNULL(B_KEY_PAIR(pair, i)) && B_KEY_CMP(B_KEY_PAIR(pair, i), key) < 0) 
		{
			break;
		}
	}

	i++;

	for (j = max - 1; j > i; j--) 
	{
		B_PAIR_COPY(pair, pair, j, j - 1);
		B_PAIR_INIT(pair, j - 1);
	}

	if (B_KEY_ISNULL(B_KEY_PAIR(pair, i))) 
	{
		B_KEY_COPY(B_KEY_PAIR(pair, i), key);
		B_ITEM_COPY(B_ITEM_PAIR(pair, i), value);	
	}

	return 0;
}

#if 0
void nvfuse_make_pair(key_pair_t *pair, inode_t ino, lbno_t lbno, u32 item, s32 *count, u32 type) {
	bkey_t key = 0;
	(*count)++;
	bp_merge_key2(pair, nvfuse_make_key(ino, lbno, &key, type), &item, *count);
}
#endif
#if 0
bkey_t *nvfuse_make_key(inode_t ino, lbno_t lbno, bkey_t *key, u32 type)
{
	type <<= (NVFUSE_BP_HIGH_BITS - NVFUSE_BP_TYPE_BITS);

	*key = ((u64)(type | ino) << NVFUSE_BP_HIGH_BITS) + (u64)lbno;
	return key;
}
#endif

u64 *nvfuse_make_pbno_key(inode_t ino, lbno_t lbno, u64 *key, u32 type)
{
	type <<= (NVFUSE_BP_HIGH_BITS - NVFUSE_BP_TYPE_BITS);

	*key = ((u64)(type | ino) << NVFUSE_BP_HIGH_BITS) + (u64)lbno;
	return key;
}

#if 0
u32 nvfuse_get_ino(bkey_t key) 
{
	u32 mask = ~0;

	mask >>= (NVFUSE_BP_TYPE_BITS);
	key >>= NVFUSE_BP_HIGH_BITS;

	key &= mask;

	return (u32)key;
}
#endif

key_pair_t *bp_alloc_pair(int num) 
{
	key_pair_t *pair;

	pair = (key_pair_t *)bp_malloc(sizeof(key_pair_t));
	if (pair == NULL) {
		printf(" malloc error \n");
		return NULL;
	}

	pair->i_key = (bkey_t *)bp_malloc(BP_KEY_SIZE * num);
	if (pair->i_key == NULL) {
		printf(" malloc error \n");
		return NULL;
	}
	memset(pair->i_key, 0x00, BP_KEY_SIZE * num);

	pair->i_item = (bitem_t *)bp_malloc(BP_ITEM_SIZE * num);
	if (pair->i_item == NULL) {
		printf(" malloc error \n");
		return NULL;
	}
	memset(pair->i_item, 0x00, BP_ITEM_SIZE * num);

	return pair;
}

void bp_release_pair(key_pair_t *pair) 
{
	bp_free((void *)pair->i_item);
	bp_free((void *)pair->i_key);
	bp_free((void *)pair);
}

void bubble_sort(key_pair_t *pair, int num, int(*compare)(void *src1, void *src2))
{
	int i, j;
	int swap = 0;

	//sorting 
	for (i = 0; i < num; i++) {
		swap = 0;
		for (j = 1; j <num - i; j++) {
			if (compare(&pair->i_key[j - 1], &pair->i_key[j]) > 0) {
				bkey_t *temp;
				bitem_t *r;

				temp = B_KEY_ALLOC();
				B_KEY_COPY(temp, &pair->i_key[j]);
				B_KEY_COPY(&pair->i_key[j], &pair->i_key[j - 1]);
				B_KEY_COPY(&pair->i_key[j - 1], temp);
				B_KEY_FREE(temp);

				r = B_ITEM_ALLOC();
				B_ITEM_COPY(r, &pair->i_item[j]);
				B_ITEM_COPY(&pair->i_item[j], &pair->i_item[j - 1]);
				B_ITEM_COPY(&pair->i_item[j - 1], r);
				B_ITEM_FREE(r);
				swap = 1;
			}
		}
		if (swap == 0)
			break;
	}
}
