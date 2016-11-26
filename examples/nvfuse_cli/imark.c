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
#include <fcntl.h>
#include <string.h>
#include <time.h>
#ifndef __linux__
#include <windows.h>
#endif 

#include "nvfuse_bp_tree.h"
#include "nvfuse_core.h"
#include "nvfuse_indirect.h"
#include "nvfuse_buffer_cache.h"

unsigned long genrand();

#define NODE_SIZE CLUSTER_SIZE
#define RND(x) ((x>0)?(genrand() % (x)):0)

/*	statistic */
int nodes_deleted = 0;
int nodes_created = 0;

/* workload table */
char  *ntable;
int	ntable_size;
//int ntable_ptr = 0;

bkey_t *workload;

unsigned int del_ptr = 0;
unsigned int cret_ptr = 0;
unsigned int read_ptr = 0;
unsigned int update_ptr = 0;

/* time */
int insert_time, transaction_time, delete_time;
time_t start_time, end_time;

/*	parameters */
#define IMARK_BUFSZ 128
unsigned int num_index = 5000000; 
int num_transaction = 5000000;
static int bias_read = -1;
static int bias_create = 5;
static int rand_workload = 0;
int lru_num = 100;
char conf_filename[IMARK_BUFSZ];
char report_filename[IMARK_BUFSZ];
//char DISK_NAME[IMARK_BUFSZ] =  "/dev/sdc1";
int use_ramdisk = 0;
int use_file = 1;
int use_device = 0;
int sync_io = 0;

#ifdef __linux__
	int open_flag = O_CREAT | O_RDWR;
	int open_flag_device = O_RDWR;
#else
	int open_flag = O_CREAT | O_RDWR;
	int open_flag_device = O_RDWR;
#endif 

int read_configure(char *filename)
{
	char line[256], *tok;
	FILE *fp;	

	fp = fopen(filename, "r");
	if(fp == NULL)
	{
		printf("cannot open %s\n", filename);
		return 0;
	}
		
	while (!feof(fp))
	{
		fgets(line, 256, fp);
		if(line[0] == '#')
			continue;

		if(!strncmp(line,"RAMDISKIO",9))
		{			
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

			use_ramdisk = atoi(tok);
		}
		
		if(!strncmp(line,"SYNCIO",6))
		{			
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

			sync_io = atoi(tok);
		}		
				
		if(!strncmp(line,"DEVICEIO",8))
		{			
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

			use_device = atoi(tok);
		}

		if(!strncmp(line,"DEVICE_NAME",11))
		{
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

		//	memset(DISK_NAME, 0x00, IMARK_BUFSZ);			
		//	strncpy(DISK_NAME, tok, strlen(tok)-1);

#ifdef __linux__
			//DISK_NAME[strlen(tok)-2] = '\0';
#endif 
		}

		if(!strncmp(line,"FILENAME",8))
		{
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

			memset(conf_filename, 0x00, IMARK_BUFSZ);			
			strncpy(conf_filename, tok, strlen(tok)-1);

#ifdef __linux__
			conf_filename[strlen(tok)-2] = '\0';
#endif 
		}
		
		if(!strncmp(line,"FILEIO",6))
		{			
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

			use_file = atoi(tok);
		}

		if(!strncmp(line,"REPORT",6))
		{			
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");

			memset(report_filename, 0x00, IMARK_BUFSZ);
			strncpy(report_filename, tok, strlen(tok)-1);

#ifdef __linux__
			report_filename[strlen(tok)-2] = '\0';
#endif 			
		}


		if(!strncmp(line,"NRINDEX",7)){
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");			
			num_index = atoi(tok);
		}
		
		if(!strncmp(line,"NRTRANSACTION", 13)){
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");
			num_transaction = atoi(tok);
		}
		if(!strncmp(line,"BIASREAD", 8)){
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");
			bias_read = atoi(tok);
		}	
		if(!strncmp(line,"BIASCREATE", 10)){
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");
			bias_create = atoi(tok);
		}	
		
		if(!strncmp(line,"RANDWORKLOAD", 12)){
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");
			rand_workload = atoi(tok);
		}
				
		if(!strncmp(line,"NRLRU", 5)){
			tok = strtok(line,"\t");
			tok = strtok(NULL,"\t");
			tok = strtok(NULL,"\t");
			lru_num = atoi(tok);
		}			

		memset(line, 0x00, 256);
	}

	return 0;

}

int check_configuration()
{
	printf(" cofiguration file name %s\n", conf_filename);
	printf(" report file name %s\n",report_filename);
	if(num_index >= BP_MAX_INDEX_NUM)
	{
		printf(" num_index(%d) is greater than %d\n", num_index, BP_MAX_INDEX_NUM);
		return -1;
	}
	if(lru_num < 1000)
	{
		printf(" lru page number (%d) is smaller than %d\n", lru_num, 1000);
		return -1;
	}
		
	printf(" no of transactions = %d\n", num_transaction);
	printf(" bias read = %d\n", bias_read);
	printf(" bias create = %d\n", bias_create);
	printf(" randon workload = %d\n", rand_workload);
	
	return 0;
}

bkey_t find_free_rec() 
{	
	unsigned long rand_num;

	while (1)
	{
		rand_num = RND((unsigned long)num_index << 1);
		if(!test_bit(ntable, rand_num)){
			set_bit(ntable, rand_num);
			return rand_num+1;
		}
	}
}

bkey_t find_used_rec() 
{
	unsigned long rand_num;

	while (1)
	{
		rand_num = RND((unsigned long)num_index << 1);
		if(test_bit(ntable, rand_num)){			
			return rand_num+1;
		}
	}

	return 0;
}

void clear_used_rec(int i)
{
	clear_bit(ntable, i-1);
}

void generate_workload()
{
	int i;
	
	printf(" workload table size = %fMB\n", (float)(num_index * sizeof(bkey_t)<<1)/(float)(1024*1024));
	workload = malloc(num_index * sizeof(bkey_t) << 1);
	if(workload == NULL)
	{
		printf(" malloc error");
		return;
	}
	memset(workload, 0x00, num_index * sizeof(bkey_t)<<1);

	for(i = 0;i < (num_index<<1);i++)
	{
		if(rand_workload)
			workload[i] = find_free_rec();
		else
			workload[i] = i + 1;
	}

	return;

}

void free_workload()
{
	free(workload);
}

bkey_t create_workload()
{
	if(cret_ptr == (num_index << 1))
		cret_ptr = 0;
	return workload[cret_ptr++];
}

bkey_t delete_workload()
{
	if(del_ptr == (num_index << 1))
		del_ptr = 0;
	return workload[del_ptr++];
}


bkey_t read_workload()
{
	
	if(read_ptr == cret_ptr)
		read_ptr = 0;

	return workload[read_ptr++];
}

bkey_t update_workload()
{
	if(update_ptr == cret_ptr)
		update_ptr = 0;
	return workload[update_ptr++];
}

int alloc_ntable() 
{
	int bit_bytes;

	ntable_size = bit_bytes = (num_index>>3) + 1;

	ntable = malloc(bit_bytes<<1);
	if(ntable == NULL)
	{
		printf(" malloc error \n");
		return -1;
	}
		
	memset(ntable, 0x00, bit_bytes<<1);


	printf(" ntable size = %fMB\n", (float)(bit_bytes<<1)/(float)(1024*1024));

	generate_workload();

	return 0;
}

int make_index(master_node_t *master){	
	int i, j;
	int ret; 
	int count = 0;
	bkey_t *key;//, *key2;

	key = B_KEY_ALLOC();
	B_KEY_INIT(key);
	
	//make index 
	for(i = 0;i < num_index;i++){
		int rand_num;
			
		rand_num = create_workload();		
		B_KEY_MAKE(key, rand_num);
		ret = B_INSERT(master, key,(bitem_t *)key, NULL, 0);
		if(i % 10000 == 0)
			printf(" insert %3d%% (%7d keys) \r",i / (num_index/100), i);		
		count ++;
				
		if(ret == -1){
			printf(" can not insert %lu", (long)*key);
			continue;
		}
	
		nvfuse_check_flush_dirty(master->m_sb, DIRTY_FLUSH_FORCE);
		
		nodes_created++;
	}

	return 0;
}

static void transactions(master_node_t *master)
{
	int i, j;
	bkey_t *key;
	int ret;
	key_pair_t *pair;
	bkey_t rand_num;

	key = B_KEY_ALLOC();
	B_KEY_INIT(key);

	for(i = 0;i < num_transaction;i++){
		if (bias_read!=-1) 
		{
			if (RND(10)<bias_read){ /* read file */				
				B_KEY_MAKE(key, read_workload());				
				if(B_SEARCH(master, key, NULL) < 0){
					printf(" cannot find [%lu]\n\n", (long)*key);
					continue;
				}
				B_FLUSH_STACK(master);
				B_RELEASE_BH(master, master->m_cur->i_bh);
				B_RELEASE(master, master->m_cur);
			}else{ /* update*/
				B_KEY_MAKE(key, update_workload());				
				ret = B_UPDATE(master, key, (bitem_t *)key);
				if(ret == -1){
					printf(" cannot update [%lu]\n\n", (long)*key);
					continue;
				}
				nvfuse_check_flush_dirty(master->m_sb, DIRTY_FLUSH_FORCE);
			}
		}		
		if(i % 10000 == 0)
			printf(" 1st transactions %3d%% (%7d keys)\r",i / (num_transaction/100), i);
	}
	
	printf("\n");

	for (i = 0;i < num_transaction;i++) {	
		if (bias_create!=-1)
		{
			if(RND(10) < bias_create 
				&& nodes_created - nodes_deleted < num_index){ /* create file */
				
				rand_num = create_workload();				
				B_KEY_MAKE(key, rand_num);					
				ret = B_INSERT(master, key,(bitem_t *) key, NULL, 0);
				if(ret == -1){
					printf(" cannot insert %lu \n\n", (long)*key);
					continue;
				}else{					
					nodes_created++;
				}				
			}else{ /* delete file */
				rand_num = delete_workload();				
				B_KEY_MAKE(key, rand_num);
				ret = B_REMOVE(master, key);
				if(ret == -1){
					printf(" cannot delete [%d] key %lu\n\n", i , (long)*key);
				}else{
					nodes_deleted++;
				}
			}
			nvfuse_check_flush_dirty(master->m_sb, DIRTY_FLUSH_FORCE);
		}
		if(i % 10000 == 0)
			printf(" 2nd transactions %3d%% (%7d keys)\r",i / (num_transaction/100), i);
	}
	B_KEY_FREE(key);
}

void delete_index(master_node_t *master)
{
	int i;
	bkey_t *key;
	int ret; 
	int total = nodes_created - nodes_deleted;
	//struct nvfuse_superblock *sb;

	key = B_KEY_ALLOC();
	B_KEY_INIT(key);

	for(i = 0;i < total;i++){
		int rand_num = i;
				
		rand_num = delete_workload();
		
		if(rand_num  == 0)
			continue;

		//sb = nvfuse_read_super(WRITE, 0);

		B_KEY_MAKE(key, rand_num);	

		ret = B_REMOVE(master, key);
		clear_used_rec(rand_num);
		
		//nvfuse_release_super(sb);

		if(ret == -1)			
			printf(" [%d] %lu is not deleted\n", i, (long)*key);

		if(i % 10000 == 0)
			printf(" delete %3d%% (%7d keys)\r",i/(total/100), i);
		nvfuse_check_flush_dirty(master->m_sb, DIRTY_FLUSH_FORCE);
	}
	
	B_KEY_FREE(key);
}

void printf_report(master_node_t *master, char *str)
{	
	FILE *fp;

	if(str == NULL)
	{
		fp = stdout;
	}else
	{
		fp = fopen(str, "w");
		if(fp == NULL)
		{
			printf(" cannot write file %s\n", str);
			return;
		}
	}	
	fprintf(fp, " \n");

	fprintf(fp, " %d way B + tree was tested!\n", master->m_fanout);
	fprintf(fp, " insert time %d sec\n", insert_time);
	fprintf(fp, " insert %.0f iops\n\n", (float)num_index/(float)insert_time);
	fprintf(fp, " transaction time %d sec\n", transaction_time);
	fprintf(fp, " transaction %.0f iops\n\n", (float)(num_transaction * 2)/(float)transaction_time);
	fprintf(fp, " delete time %d sec\n", delete_time);
	fprintf(fp, " delete %.0f iops\n\n", (float)(nodes_created - nodes_deleted)/(float)delete_time);
	fprintf(fp, " total time %d sec \n", insert_time + transaction_time + delete_time);
	
	fprintf(fp, " \n");

	if(str != NULL)
	{
		fclose(fp);
	}
}

time_t diff_time(time_t, time_t);

extern struct nvfuse_handle *g_nvh;

int imark_main(int argc, char *argv[]) {
	int ret, rand_count = 0;
	master_node_t _master;
	master_node_t *master = &_master;
	struct nvfuse_superblock *sb = nvfuse_read_super(g_nvh);
	struct nvfuse_inode *inode;	
	printf(" iMark for B+tree Test and Validation (Ver 0.1) \n");
	printf(" Developed by Yongseok Oh (Yongseok Oh@sk.com)\n");

	printf("\n ============== Node Size ============== \n");

	//printf(" Node Size in-memory = %d byte\n",sizeof(index_node_t));
	printf(" Node Size on-disk = %d byte\n", NODE_SIZE);

	/*	read configure file */
#ifndef __linux__
	read_configure("../imark_config/win.conf");
#else
	if (argc < 2) {
		printf("\n	usage :./imark  ConfFile \n\n");
		return 0;
	}
	read_configure(argv[1]);
#endif 

	if (check_configuration() < 0)
	{
		return -1;
	}

	printf("\n ============== Parameters ============== \n");
	printf(" cofiguration file name %s\n", conf_filename);
	printf(" report file name %s\n", report_filename);
	printf(" no of index = %d\n", num_index);
	printf(" no of transactions = %d\n", num_transaction);
	printf(" bias read = %d\n", bias_read);
	printf(" bias create = %d\n", bias_create);
	printf(" randon workload = %d\n", rand_workload);
	printf(" no of lru cache = %d\n", lru_num);
	printf(" device file = %s\n", DISK_NAME);
	printf(" use ramdisk io  = %d\n", use_ramdisk);
	printf(" use file io = %d\n", use_file);
	printf(" use device io = %d\n", use_device);
	printf(" use sync io = %d\n", sync_io);

	printf("\n ============== Format B+-tree ============== \n");
	/* create master node and root node through making new inode on file system */
	{
		master = bp_init_master();				
		bp_alloc_master(sb, master);
		bp_init_root(master);

		nvfuse_check_flush_dirty(sb, DIRTY_FLUSH_FORCE);
	}

	alloc_ntable();
			
	printf("\n ============== Make B+-tree ============== \n");
	/* make index */
	time(&start_time);
	make_index(master);
	time(&end_time);
	insert_time = diff_time(end_time, start_time);
	printf("\n");
		
	printf("\n ============== Run ============== \n");
	/* transaction */
	time(&start_time);
		transactions(master);
	time(&end_time);	
	transaction_time = diff_time(end_time, start_time);
	printf("\n");
	
	printf("\n ============== Delete B+-tree ============== \n");
	/* delete index */
	time(&start_time);
		delete_index(master);	
	time(&end_time);	
	delete_time = diff_time(end_time, start_time);
	printf("\n");
	
	/* report */
	printf("\n ============== Report ============== \n");
		printf_report(master, NULL);		
		printf_report(master, report_filename);

	/* delete allocated b+tree inode */
	{	
		struct nvfuse_inode_ctx *ictx;
		ictx = nvfuse_read_inode(master->m_sb, NULL, master->m_ino);
		nvfuse_free_inode_size(sb, ictx, 0);
		nvfuse_relocate_delete_inode(sb, ictx);
	}

	free_workload();

	return 0;
}
