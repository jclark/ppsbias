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
# Rockchip GPIO v2 banks 0 and 3 (aliases gpio0, gpio3); ppsrk is line 18 of bank 0,
# ppsrk3 is line 3 of bank 3 (GPIO3_A3 = 99).
mkdir -p dt/aliases dt/pinctrl/gpio@fdd60000 dt/pinctrl/gpio@fe760000 dt/ppsrk dt/ppsrk3
printf '/pinctrl/gpio@fdd60000\0' > dt/aliases/gpio0
printf '/pinctrl/gpio@fe760000\0' > dt/aliases/gpio3
{ cell 0; cell 0xfdd60000; cell 0; cell 0x100; } > dt/pinctrl/gpio@fdd60000/reg
{ cell 0; cell 0xfe760000; cell 0; cell 0x100; } > dt/pinctrl/gpio@fe760000/reg
cell 0x22 > dt/pinctrl/gpio@fdd60000/phandle
cell 0x53 > dt/pinctrl/gpio@fe760000/phandle
{ cell 0x22; cell 18; cell 0; } > dt/ppsrk/gpios
{ cell 0x53; cell 3; cell 0; } > dt/ppsrk3/gpios
for dev in ppsrk ppsrk3; do
	mkdir -p sys/class/pps/$dev sys/bus/platform/devices/$dev
	echo "$dev.-1" > sys/class/pps/$dev/name
	ln -s ../../../../../dt/$dev sys/bus/platform/devices/$dev/of_node
done
# TI K3 Keystone GPIO main_gpio1 (phandle 0x31); ppsk3 is its line 18.
# ppsk3bad is line 18 of the controller labelled gpio, which is not a Keystone GPIO.
mkdir -p dt/bus/gpio@601000 dt/ppsk3 dt/ppsk3bad
printf '/bus/gpio@601000\0' > dt/__symbols__/main_gpio1
printf 'ti,am64-gpio\0ti,keystone-gpio\0' > dt/bus/gpio@601000/compatible
{ cell 0; cell 0x601000; cell 0; cell 0x100; } > dt/bus/gpio@601000/reg
cell 0x31 > dt/bus/gpio@601000/phandle
{ cell 0x31; cell 18; cell 0; } > dt/ppsk3/gpios
{ cell 7; cell 18; cell 0; } > dt/ppsk3bad/gpios
for dev in ppsk3 ppsk3bad; do
	mkdir -p sys/class/pps/$dev sys/bus/platform/devices/$dev
	echo "$dev.-1" > sys/class/pps/$dev/name
	ln -s ../../../../../dt/$dev sys/bus/platform/devices/$dev/of_node
done
