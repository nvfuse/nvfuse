#!/bin/sh

. ../root_check.sh

# -c: core mask
# -a: app name
# -f: format

sudo ./helloworld -f -m -c 7 -a helloworld -o nvme.conf
