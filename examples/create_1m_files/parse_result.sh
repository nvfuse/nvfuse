#!/bin/sh

OUTPUT=result

# Mill Test
for type in dynamic static
do
	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg Container Alloc Latency' $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg Container Free Latency' $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg BUFFER Alloc Latency' $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg BUFFER Free Latency' $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg involuntary context switches' $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg page reclaims (soft page faults' $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'Avg execution'  $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'TC 0 Execution'  $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'TC 1 Execution'  $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done

	for core_mask in 2 6 14 30 62 126 254
	do
		ret=$(grep 'TC 2 Execution'  $OUTPUT/${type}_result_core_mask_${core_mask}.txt)
		echo coremask $core_mask $ret
	done
done
