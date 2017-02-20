#!/bin/bash

FIO_MINIMAL_DECODER=~/fio_minimal_decoder/fio_minimal_output_decode.py
OUTPUT_PATH=output 
ARRANGE_PATH=arrange
PREFIXS=(kernel_ext2)
PREFIX2=async
#kernel_xfs)

for PREFIX in "${PREFIXS[@]}"
do

    READ_IOPS_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_read_iops.log
    WRITE_IOPS_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_write_iops.log

    READ_LAT_AVG_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_read_lat_avg.log
    WRITE_LAT_AVG_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_write_lat_avg.log

    READ_LAT_MIN_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_read_lat_min.log
    WRITE_LAT_MIN_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_write_lat_min.log

    READ_LAT_MAX_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_read_lat_max.log
    WRITE_LAT_MAX_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_write_lat_max.log

    READ_CPU_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_read_cpu.log
    WRITE_CPU_LOG=${OUTPUT_PATH}/${ARRANGE_PATH}/${PREFIX}_${PREFIX2}_write_cpu.log

    rm $READ_IOPS_LOG
    rm $WRITE_IOPS_LOG

    rm $READ_LAT_AVG_LOG
    rm $WRITE_LAT_AVG_LOG

    rm $READ_LAT_MIN_LOG
    rm $WRITE_LAT_MIN_LOG

    rm $READ_LAT_MAX_LOG
    rm $WRITE_LAT_MAX_LOG 

    rm $READ_CPU_LOG
    rm $WRITE_CPU_LOG

    #for workload in read randread
    for workload in randread randwrite
    do
        for numjobs in {1..6} 
        do
            for qdepth in 1 2 4 8 16 32 64 128 256
            do
                if [ $workload = read -o $workload = write  ] ; then 
                    block_size=$((64*1024))
                else
                    block_size=$((4096))
                fi

                output=${OUTPUT_PATH}/${PREFIX}_${PREFIX2}_numjobs_${numjobs}_q_${qdepth}_block_${block_size}_workload_${workload}.log
                if [ $workload = randread  ] ; then 

                    # IOPS
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_iops | head -n 1)
                    echo $ret
                    echo qd $qdepth  $ret >> ${READ_IOPS_LOG}

                    # AVG LAT 
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_lat_mean)
                    echo $ret
                    echo qd $qdepth  $ret >> ${READ_LAT_AVG_LOG}

                    # MAX LAT 
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_lat_max)
                    echo $ret
                    echo qd $qdepth  $ret >> ${READ_LAT_MAX_LOG}

                    # MIN LAT 
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_lat_min)
                    echo $ret
                    echo qd $qdepth  $ret >> ${READ_LAT_MIN_LOG}


                    ret1=$($FIO_MINIMAL_DECODER $output | grep cpu_user | awk '{print "cpu_usr" "\t"$3}')
                    ret2=$($FIO_MINIMAL_DECODER $output | grep cpu_sys | awk '{print "cpu_sys" "\t"$3}')
                    ret3=$($FIO_MINIMAL_DECODER $output | grep cpu_csw | awk '{print "cpu_csw" "\t"$3}')
                    ret4=$($FIO_MINIMAL_DECODER $output | grep cpu_mjf | awk '{print "cpu_majf" "\t"$3}')
                    ret5=$($FIO_MINIMAL_DECODER $output | grep pu_minf | awk '{print "cpu_minf" "\t"$3}')
                    echo $ret1 $ret2 $ret3 $ret4 $ret5
                    echo qd $qdepth thread $numjobs  $ret1 $ret2 $ret3 $ret4 $ret5 >> ${READ_CPU_LOG}
                else

                    # IOPS
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_iops | head -n 1)
                    echo $ret
                    echo qd $qdepth  $ret >> ${WRITE_IOPS_LOG}

                    # AVG LAT 
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_lat_mean)
                    echo $ret
                    echo qd $qdepth  $ret >> ${WRITE_LAT_AVG_LOG}

                    # MAX LAT 
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_lat_max)
                    echo $ret
                    echo qd $qdepth  $ret >> ${WRITE_LAT_MAX_LOG}

                    # MIN LAT 
                    ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_lat_min)
                    echo $ret
                    echo qd $qdepth  $ret >> ${WRITE_LAT_MIN_LOG}

                    ret1=$($FIO_MINIMAL_DECODER $output | grep cpu_user | awk '{print "cpu_usr" "\t"$3}')
                    ret2=$($FIO_MINIMAL_DECODER $output | grep cpu_sys | awk '{print "cpu_sys" "\t"$3}')
                    ret3=$($FIO_MINIMAL_DECODER $output | grep cpu_csw | awk '{print "cpu_csw" "\t"$3}')
                    ret4=$($FIO_MINIMAL_DECODER $output | grep cpu_mjf | awk '{print "cpu_majf" "\t"$3}')
                    ret5=$($FIO_MINIMAL_DECODER $output | grep pu_minf | awk '{print "cpu_minf" "\t"$3}')
                    echo $ret1 $ret2 $ret3 $ret4 $ret5
                    echo qd $qdepth thread $numjobs  $ret1 $ret2 $ret3 $ret4 $ret5 >> ${WRITE_CPU_LOG}
                fi
            done
        done
    done

    cat $READ_IOPS_LOG
    cat $WRITE_IOPS_LOG

    cat $READ_LAT_AVG_LOG
    cat $WRITE_LAT_AVG_LOG

    cat $READ_LAT_MIN_LOG
    cat $WRITE_LAT_MIN_LOG

    cat $READ_LAT_MAX_LOG
    cat $WRITE_LAT_MAX_LOG
done
