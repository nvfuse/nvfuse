#!/bin/sh

sed '/kernel.randomize_va_space/d' /etc/sysctl.conf > tmp.conf
echo 'kernel.randomize_va_space = 0' >> tmp.conf
mv tmp.conf /etc/sysctl.conf
sysctl -p
