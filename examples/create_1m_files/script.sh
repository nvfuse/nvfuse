#!/bin/sh

OUTPUT_PATH=result
if [ ! -d $OUTPUT_PATH ] ; then
	mkdir -p $OUTPUT_PATH
fi

# Mill Test
for type in dynamic static
do
	for core_mask in 1 2 3 4 5 6 7
	do
		if  [ $type = static ] ; then
			prealloc="-p"
		else
			prealloc=" "
		fi

		core_mask=$(((1 << core_mask) - 1 << 1))

		str="./create_1m_files -c $core_mask -a test $prealloc | tee $OUTPUT_PATH/${type}_result_core_mask_${core_mask}.txt"
		echo $str
		eval $str
	done
done

OUTPUT_PATH=perf_result
if [ ! -d $OUTPUT_PATH ] ; then
	mkdir -p $OUTPUT_PATH
fi

for type in dynamic static
do
	for core_mask in 1 2 3 4 5 6 7
	do
		if  [ $type = static ] ; then
			prealloc="-p"
		else
			prealloc=" "
		fi

		core_mask=$(((1 << core_mask) - 1 << 1))

		str="perf stat -d -d -d ./create_1m_files -c $core_mask -a test $prealloc | tee $OUTPUT_PATH/${type}_result_core_mask_${core_mask}.txt"
		echo $str
		eval $str
	done
done
