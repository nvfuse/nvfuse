#!/bin/sh

cmd="sudo ./control_plane_proc -f -m -b 4096 -c 1"

echo $cmd

eval $cmd
