#!/bin/sh
# Build the original fstests fsx at a fixed revision; run inside the Linux VM.
set -eu
if [ "$(uname -s)" != Linux ]; then
    echo 'Run this setup inside the Linux QEMU guest.' >&2
    exit 1
fi
revision=d07dca72d2455f4ba8c660ad4c008541ccccb707
reference_dir=${1:-"$HOME/src/fs-reference/xfstests"}
if [ ! -d "$reference_dir" ]; then
    mkdir -p "$reference_dir"
    git -C "$reference_dir" init
    git -C "$reference_dir" remote add origin https://github.com/kdave/xfstests.git
    git -C "$reference_dir" fetch --depth 1 origin "$revision"
    git -C "$reference_dir" checkout --detach FETCH_HEAD
fi
if [ "$(git -C "$reference_dir" rev-parse HEAD)" != "$revision" ]; then
    echo "Existing checkout must be at $revision; use a new destination." >&2
    exit 1
fi
git -C "$reference_dir" diff --exit-code HEAD --
cd "$reference_dir"
make configure
./configure
make lib
make -C ltp fsx
printf 'fsx binary: %s/ltp/fsx\n' "$reference_dir"
git rev-parse HEAD
sha256sum ltp/fsx
