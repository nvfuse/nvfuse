NVFUSE
======

[![Build Status](https://travis-ci.org/nvfuse/nvfuse.svg?branch=master)](https://travis-ci.org/nvfuse/nvfuse)

NVFUSE is an embeddable file system as a library running in the user-space incorporated with [SPDK library](https://github.com/spdk/spdk) which is a user-space NVMe driver newly introduced by Intel. Using this file system applications can directly submit I/O requests to NVMe SSDs. Then, it provides high performance, high reliability as well as POSIX-like interfaces (e.g., nvfuse_open, nvfuse_read, nvfuse_write, and nvfuse_close functions). Note that this file system doesn't make use of well-known FUSE (File System Userspace) library to provide POSIX compatibility. Key features are listed as follows.

 - Running in userspace without any interactions with kernel driver so that interrupts, context switches, and memory copies are eliminated. 
 - Embeddable file system as a library that introduces new interfaces are similar to POSIX API.
 - Simple file system layout that is identical to EXT2 file system.
 - Strong file system consistency and durability without pessimistic journaling mechanism (e.g., EXT4 journaling) through NVMe metadata feature as described [NVMe spec](http://nvmexpress.org/wp-content/uploads/NVM_Express_1_2_Gold_20141209.pdf). 

Prerequisites
=============
In order to make NVFUSE, some relevant libraries must be configured. 

[DPDK](http://dpdk.org/doc/quick-start) and [SPDK](https://github.com/spdk/spdk) are required.

    1) Move onto https://github.com/spdk/spdk
    2) DPDK and SPDK must be installed based on thier install guide explained in README.

Ubuntu/Debian:

    # sudo apt-get install -y gcc make libaio-dev libfuse-dev

Building
========

Once the prerequisites are installed, run 'make' within the NVFUSE directory to build the NVFUSE library and examples.

    # make SPDK_ROOT_DIR=/home/spdk DPDK_DIR=/home/spdk/dpdk-16.11/x86_64-native-linuxapp-gcc

Move to examples directory and execute helloworld. 

    # cd examples/helloworld
    # ./helloworld

To do
=====

 - Multi thread support 
 - Multi NVMe queue support

Contact
=======
Yongseok Oh (yongseok.oh@sk.com)
