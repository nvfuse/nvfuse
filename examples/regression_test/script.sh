#!/bin/sh

# MAX Test
#sudo ./regression_test -t spdk -d 01:00 -f -m -T 1
# Quick Test
#sudo ./regression_test -t spdk -d 01:00 -f -m -T 2
# Mill Test
sudo ./regression_test -t spdk -d 01:00 -f -m -T 3
