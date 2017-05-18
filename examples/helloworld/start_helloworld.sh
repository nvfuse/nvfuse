#!/bin/sh

. ../root_check.sh

# -c: core mask
# -a: app name
# -f: format

sudo ./helloworld -f -c 2 -a helloworld
