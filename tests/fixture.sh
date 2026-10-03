#!/bin/sh
# Build a synthetic /sys and /proc/device-tree under $1 for the PPS GPIO lookup.
# pps0, pps1 and fakepps are pps-gpio devices on line 18 of the controller
# labelled gpio and rp1_gpio (phandle 7); ppslow is the same line active low.
set -e
cd "$1"
mkdir -p dt/__symbols__ dt/soc/gpio dt/pps sys/class/pps sys/bus/platform/devices
cell() { printf "\\$(printf %o $(($1 >> 24 & 255)))\\$(printf %o $(($1 >> 16 & 255)))\\$(printf %o $(($1 >> 8 & 255)))\\$(printf %o $(($1 & 255)))"; }
printf '/soc/gpio\0' > dt/__symbols__/gpio
printf '/soc/gpio\0' > dt/__symbols__/rp1_gpio
cell 7 > dt/soc/gpio/phandle
for dev in pps0 pps1 fakepps; do mkdir -p sys/class/pps/$dev; echo 'pps@12.-1' > sys/class/pps/$dev/name; done
mkdir -p sys/class/pps/ppslow dt/ppslow sys/bus/platform/devices/pps@12 sys/bus/platform/devices/ppslow
echo 'ppslow.-1' > sys/class/pps/ppslow/name
{ cell 7; cell 18; cell 0; } > dt/pps/gpios
{ cell 7; cell 18; cell 1; } > dt/ppslow/gpios
ln -s ../../../../../dt/pps sys/bus/platform/devices/pps@12/of_node
ln -s ../../../../../dt/ppslow sys/bus/platform/devices/ppslow/of_node
