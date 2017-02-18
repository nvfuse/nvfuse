#!/bin/bash

WORKING_DIR=/home/son/nvfuse/nvfuse/examples/perf/perf_kernel
MOUNT_DIR=$WORKING_DIR/mnt
DEV=/dev/nvme0n1
PGM=$WORKING_DIR/kernel_fs
RESULT_DIR=$WORKING_DIR/result
RESULT_DIR_FIO=$RESULT_DIR/FIO 
RESULT_DIR_META=$RESULT_DIR/META

experiment_type=$1

file_systems[0]='ext2'
file_systems[1]='ext4'
file_systems[2]='xfs'

function meta_experiment(){
    local fs=$1
    for i in {1..6}
    do
        echo start $i
        echo 3 > /proc/sys/vm/drop_caches
        ./kernel_fs -M total -C 10 -T $i > $RESULT_DIR_META/${file_systems[$fs]}_perf_th_$i.txt
        echo end $i
        echo ""
    done
}

function fio_experiment(){

    # 4k random write
    local fs=$1
    for i in {1..6}
    do
        fio --name=sync --directory=. --ioengine=sync --size=32g --bs=4k --fsync=1 --rw=randwrite --thread=1 --numjobs=${i} --group_reporting --output=$RESULT_DIR_FIO/${file_systems[fs]}_4kb_syncio_fsync_write_th_${i}.txt
    done

    # 4k random read
    for i in {1..6}
    do
        fio --name=sync --directory=. --ioengine=sync --size=32g --bs=4k -rw=randread --thread=1 --numjobs=${i} --group_reporting --output=$RESULT_DIR_FIO/${file_system[fs]}_kb_syncio_fsync_read_th_${i}.txt
    done
}

function init(){
    if [ ! -d $MOUNT_DIR ]
    then
        mkdir $MOUNT_DIR
    fi 

    if [ ! -d $RESULT_DIR ]
    then
        mkdir $RESULT_DIR
    fi 

    if [ ! -d $RESULT_DIR_FIO ]
    then
        mkdir $RESULT_DIR_FIO 
    fi 

    if [ ! -d $RESULT_DIR_META ]
    then
        mkdir $RESULT_DIR_META
    fi 

    source /home/son/nvfuse/nvfuse/scripts/setup.sh reset

    echo $((4*1024*1024)) > /proc/sys/vm/dirty_bytes
    echo $((4*1024*1024)) > /proc/sys/vm/dirty_background_bytes 
}

function destroy(){
    echo 0 > /proc/sys/vm/dirty_bytes
    echo 0 > /proc/sys/vm/dirty_background_bytes 
}

init
for i in {0..2}
do 
    if [ ${file_systems[$i]} = "xfs" ]
    then
        mkfs.${file_systems[$i]} -f $DEV  
    else
        yes | mkfs.${file_systems[$i]} $DEV
    fi 
    mount $DEV $MOUNT_DIR

    cp $PGM $MOUNT_DIR        
    cd $MOUNT_DIR
    if [ $experiment_type = "meta" ]
    then
        meta_experiment i
    elif [ $experiment_type = "fio" ]
    then
        fio_experiment i
    else
        echo "type missing"
    fi
    cd $WORKING_DIR 
    umount $MOUNT_DIR
done
destroy
    
#cp $PGM $MOUNT_DIR
#yes | mkfs.xfs $DEV
#umount $MOUNT_DIR 

#ext4 




