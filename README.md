NVFUSE
======

NVFUSE is an embeddable file system as a library running in the user-space incorporated with [SPDK library](https://github.com/spdk/spdk) which is a user-space NVMe driver newly introduced by Intel. Using this file system applications can directly submit I/O requests to NVMe SSDs. Then, it provides high performance, high reliability as well as POSIX-like interfaces (e.g., nvfuse_open, nvfuse_read, nvfuse_write, and nvfuse_close functions). Note that this file system doesn't make use of well-known FUSE (File System in Userspace) library to provide POSIX compatibility. Key features are listed as follows.

 - Running in userspace without any interactions with kernel driver so that interrupts, context switches, and memory copies are eliminated. 
 - Embeddable file system as a library that introduces new interfaces are similar to POSIX API.
 - Simple file system layout that is identical to EXT family file system (e.g., EXT3).
 - Strong file system consistency and durability without pessimistic journaling mechanism (e.g., EXT4 journaling) through NVMe metadata feature as described [NVMe spec](http://nvmexpress.org/wp-content/uploads/NVM_Express_1_2_Gold_20141209.pdf). 

Local development on macOS and Linux
===================================

A separate file-image backend supports macOS and Linux development without SPDK,
DPDK, hugepages, root access, or FUSE. It runs the existing NVFUSE filesystem,
including its metadata, buffer cache and directory indexes, against a regular
file using `pread`, `pwrite` and `fsync`.

Requirements: `clang`, `make` and Python 3.9 or later. On macOS, install Xcode
Command Line Tools. Verified on Apple Silicon macOS and Rocky Linux 9 ARM64 in
Colima. Recent SPDK support remains follow-up work.

    make local
    make local-test
    make local-sanitize

`local-test` creates a disposable 256 MiB sparse image, then runs separate
processes to create and verify a binary file larger than 3 MiB, perform an
unaligned overwrite, verify 40 small files, rename and delete entries, and verify
those changes after remount. It also checks repeated mounts within one process
and rejects existing create targets, locked images, symlinks and invalid image
headers. Temporary images are removed automatically; failures exit nonzero.
`local-sanitize` runs the same tests with AddressSanitizer and
UndefinedBehaviorSanitizer. Legacy source warnings remain visible; the new
backend and test executable compile with `-Werror`.

Pull requests run `make local-test` on a GitHub-hosted macOS runner and both
regular and ASan/UBSan tests in a Rocky Linux 9 container on an Ubuntu x86_64
runner through `.github/workflows/local-tests.yml`. The workflow can also be
started manually from the Actions tab. SPDK CI remains follow-up work.

To run the Rocky Linux 9 tests through an existing Colima Docker VM:

    sh scripts/test-rocky9.sh

The script builds a toolchain image with `clang`, `compiler-rt`, `make` and
Python, then runs both the regular and ASan/UBSan tests. Source is mounted read
only; build products and disposable filesystem images stay inside a temporary
container. Tests run without network access. The toolchain image is retained
for reuse. Override `NVFUSE_DOCKER_CONTEXT` to use a different Docker context.
On Apple Silicon, the default Colima VM tests Linux ARM64; the GitHub Actions
Rocky 9 job tests x86_64. Rocky 8 requires separate validation.

To keep an image for debugging, use a new path:

    ./build/local/local_test create /tmp/nvfuse-dev.img
    ./build/local/local_test verify /tmp/nvfuse-dev.img
    ./build/local/local_test mutate /tmp/nvfuse-dev.img
    ./build/local/local_test final /tmp/nvfuse-dev.img

The executable's source is `tests/local_test.c`. Applications can link
`build/local/nvfuse.a`, compile with `-DNVFUSE_FILE_BACKEND -Iinclude`, open a
filesystem with `nvfuse_file_open()` from `include/nvfuse_file.h`, call the normal
`nvfuse_*` file APIs, and finish with `nvfuse_file_close()`. Creation requires a
new file path and an image size in multiples of 128 MiB; reopening never formats
an existing image. Use a different path for another fresh image.

This backend currently supports one filesystem handle per process and calls
from one thread. I/O executes synchronously, including batched cache writeback.
It does not validate SPDK performance, multiprocess IPC, crash recovery, or
cross-platform image interchange, and does not mount a volume in Finder. Use
disposable development images: underlying block-I/O errors or partial mount
failures abort the process because the legacy core cannot reliably unwind those failures.
The original SPDK build commands below remain separate from this local mode.

Prerequisites
=============
In order to build NVFUSE, some relevant libraries must be configured. 

[DPDK](http://dpdk.org/doc/quick-start) and [SPDK](https://github.com/spdk/spdk) are required.

    1) Move onto https://github.com/spdk/spdk
    2) DPDK and SPDK must be installed based on their install guide explained in README.

Ubuntu/Debian:
	
    # sudo apt-get install -y gcc make libaio-dev libfuse-dev
	# sudo apt-get install -y gcc g++ libcunit1-dev libaio-dev libssl-dev

SPDK compile:

	# git clone https://github.com/spdk/spdk.git
	# cd ~/spdk
	# git submodule update --init
	# ./configure
	# make

Building
========

Once the prerequisites are installed, run 'make' within the NVFUSE directory to build the NVFUSE library and examples.

    # make SPDK_ROOT_DIR=/home/ysoh/spdk DPDK_DIR=/home/ysoh/spdk/dpdk/build

Before conducting an NVFUSE application, the kernel NVMe driver must be unloaded and some hugepages must be allocated. It can be done with the automation script like the below command line.

    # sudo scripts/setup.sh config

Disabling ASLR (Address Space Layout Randomization) is also required like this.

	# sudo scripts/disable_aslr.sh

Move to examples directory and execute helloworld. 

    # cd examples/helloworld
    # sudo ./start_helloworld.sh

Make use of performance benchmark.
	# cd examples/perf
	# sudo ./start_perf.sh

After finalizing an NVFUSE application, the kernel NVMe driver and hugepages can be reverted with the automation script.

    # sudo scripts/setup.sh reset

| Examples Not Working (To Be Enabled) |
|--|
|control_plane_proc|
|dpdk_simple_mp|
|fio_plugin|
|libfuse|
|multi_process|
|raw_block_device_perf|
|regression_test|
|create_1m_files|
|fio-nvfuse|
|fsync_test|
|kernel_filesystem|
|mkfs|
|reactor|

To do
=====

 - File system consistency management via light-weight journaling
 - Integration with scalable file system such as Ceph and GlusterFS

Contact
=======
Yongseok Oh (yongseok.oh@sk.com)
