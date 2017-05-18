#!/bin/sh

if [ "$(whoami)" != "root" ] ; then
	echo "Please run as root"
	echo "Press [CTRL+C] to stop.."
	while true
	do
		sleep 1
	done
fi
