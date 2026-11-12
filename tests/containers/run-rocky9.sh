#!/bin/sh
set -eu

cat /etc/rocky-release
uname -m
clang --version
python3 --version

make local-test LOCAL_BUILD=/tmp/nvfuse-build
make -f Makefile.local LOCAL_BUILD=/tmp/nvfuse-build framework-test journal-test
UBSAN_OPTIONS=halt_on_error=1 make -f Makefile.local \
    LOCAL_BUILD=/tmp/nvfuse-asan \
    LOCAL_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' test framework-test journal-test
