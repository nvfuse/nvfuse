#!/bin/bash

echo " Run aio example"
LD_PRELOAD=./fio_plugin fio example_aio.fio --filename=nvfuse.fio.aio.file

echo " Run sync example"
LD_PRELOAD=./fio_plugin fio example_sync.fio --filename=nvfuse.fio.sync.file
