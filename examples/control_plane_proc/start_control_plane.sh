#!/bin/sh

cmd="sudo ./control_plane_proc -f -m -b 16384 -c 1"

echo $cmd

eval $cmd
