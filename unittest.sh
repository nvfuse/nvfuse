#!/bin/sh

# execute helloworld example
examples/helloworld/helloworld -t ramdisk -t ramdisk -s 1024 -b 16
#
#examples/regression_test/regression_test -t ramdisk -t ramdisk -s 1024 -b 16 -T 2
cloc .
