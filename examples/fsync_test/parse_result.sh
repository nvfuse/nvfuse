#!/bin/sh

OUTPUT=result

# Mill Test
for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg Container Alloc Latency' $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg Container Free Latency' $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg BUFFER Alloc Latency' $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg BUFFER Free Latency' $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg involuntary context switches' $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg page reclaims (soft page faults' $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg execution'  $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done

for core_mask in 2 6 14 30 62 126 254
do
	ret=$(grep 'Avg bandwidth'  $OUTPUT/result_core_mask_${core_mask}.txt)
	echo coremask $core_mask $ret
done
