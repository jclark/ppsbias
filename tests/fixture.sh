#!/bin/sh
# Build a synthetic /sys and device tree under $1 for finding a PPS device's GPIO controller.
#
# GPIO controllers (phandle: backend, node, CPU address of its first register range):
#   7:    bcm2835 /soc/gpio@7e200000, Pi 3 bus ranges, 0x3f200000
#   8:    bcm2711 /soc4/gpio@7e200000, Pi 4 bus ranges, 0xfe200000
#   9:    rp1 /axi/pcie@1000120000/rp1/gpio@d0000, the Pi 5's PCIe ranges in the Raspberry Pi
#         kernel's device tree, 0x1f000d0000 (second range, RIO, 0x1f000e0000)
#   0x60: rp1 /axim/pcie@1000120000/pci@0,0/dev@0,0/pci-ep-bus@1/gpio@d0000, as in mainline's
#         device tree, where RP1's ranges go through its PCI BAR 1; the same addresses
#   0x22: rockchip /pinctrl/gpio@fdd60000, empty ranges, 0xfdd60000
#   0x53: rockchip /pinctrl/gpio@fe760000, 0xfe760000
#   0x31: k3 /bus@f0000/gpio@601000, ti,ngpio 73, 0x601000
#   0x36: mxc /soc@0/bus@30000000/gpio@30200000, i.MX 8M Plus bus ranges, 0x30200000
#   0x40: unsupported brcm,brcmstb-gpio
#   0x41: rockchip under a bus with no ranges, so untranslatable
#   0x42: rockchip with #gpio-cells 3
# PPS devices (/sys/class/pps entries) with a device-tree GPIO (phandle, line, flags):
#   pps0, pps1, fakepps: 9 18 0 (one platform device); ppslow: 9 18 1 (active low)
#   ppsbcm: 7 18; ppsbcm54: 7 54; pps2711: 8 18; ppsrk: 0x22 18; ppsrk3: 0x53 3;
#   ppsrk32: 0x22 32; ppsk3: 0x31 18; ppsk3big: 0x31 73; ppsmxc: 0x36 18;
#   ppsbad: 0x40 18; ppsnone: 0x99 18; ppsnoranges: 0x41 18; ppscells: 0x42 18 0 0;
#   ppsmain: 0x60 18
# and pps-gpio devices that are not: ptp, a PTP clock's PPS device, and ppsother, a platform
# device with a gpios property bound to another driver.
set -e
cd "$1"
cell() { for c; do printf "\\$(printf %o $((c >> 24 & 255)))\\$(printf %o $((c >> 16 & 255)))\\$(printf %o $((c >> 8 & 255)))\\$(printf %o $((c & 255)))"; done; }
bus() { mkdir -p "dt$1"; cell "$2" > "dt$1/#address-cells"; cell "$3" > "dt$1/#size-cells"; }
chip=0
# controller node phandle compatible reg...
controller() {
	node=$1 phandle=$2 compatible=$3; shift 3
	mkdir -p "dt$node"
	cell "$phandle" > "dt$node/phandle"
	cell 2 > "dt$node/#gpio-cells"
	printf "$compatible\0" > "dt$node/compatible"
	cell "$@" > "dt$node/reg"
	mkdir -p sys/bus/gpio/devices/gpiochip$chip
	ln -s "../../../../../dt$node" sys/bus/gpio/devices/gpiochip$chip/of_node
	chip=$((chip + 1))
}
bus '' 2 2
bus /soc 1 1
cell 0x7e000000 0 0x3f000000 0x01000000 > dt/soc/ranges
controller /soc/gpio@7e200000 7 brcm,bcm2835-gpio 0x7e200000 0xb4
bus /soc4 1 1
cell 0x7e000000 0 0xfe000000 0x01800000 0x7c000000 0 0xfc000000 0x02000000 \
	0x40000000 0 0xff800000 0x00800000 > dt/soc4/ranges
controller /soc4/gpio@7e200000 8 brcm,bcm2711-gpio 0x7e200000 0xb4
bus /axi 2 2
cell 0 0 0 0 0x10 0 0x10 0 0x10 0 1 0 0x14 0 0x14 0 4 0 0x18 0 0x18 0 4 0 0x1c 0 0x1c 0 4 0 > dt/axi/ranges
bus /axi/pcie@1000120000 3 2
printf 'pci\0' > dt/axi/pcie@1000120000/device_type
cell 0x02000000 0 0 0x1f 0 0 0xfffffffc 0x43000000 4 0 0x1c 0 3 0 > dt/axi/pcie@1000120000/ranges
bus /axi/pcie@1000120000/rp1 2 2
cell 0xc0 0x40000000 0x02000000 0 0 0 0x00410000 > dt/axi/pcie@1000120000/rp1/ranges
controller /axi/pcie@1000120000/rp1/gpio@d0000 9 raspberrypi,rp1-gpio \
	0xc0 0x400d0000 0 0xc000 0xc0 0x400e0000 0 0xc000 0xc0 0x400f0000 0 0xc000
bus /pinctrl 2 2
: > dt/pinctrl/ranges
controller /pinctrl/gpio@fdd60000 0x22 rockchip,gpio-bank 0 0xfdd60000 0 0x100
controller /pinctrl/gpio@fe760000 0x53 rockchip,gpio-bank 0 0xfe760000 0 0x100
bus /bus@f0000 2 2
cell 0 0x600000 0 0x600000 0 0x1100 > dt/bus@f0000/ranges
controller /bus@f0000/gpio@601000 0x31 'ti,am64-gpio\0ti,keystone-gpio' 0 0x601000 0 0x100
cell 73 > dt/bus@f0000/gpio@601000/ti,ngpio
bus /soc@0 1 1
cell 0 0 0 0x3e000000 > dt/soc@0/ranges
bus /soc@0/bus@30000000 1 1
: > dt/soc@0/bus@30000000/ranges
controller /soc@0/bus@30000000/gpio@30200000 0x36 'fsl,imx8mp-gpio\0fsl,imx35-gpio' 0x30200000 0x10000
bus /axim 2 2
cell 0 0 0 0 0x10 0 0x10 0 0x10 0 1 0 0x14 0 0x14 0 4 0 0x18 0 0x18 0 4 0 0x1c 0 0x1c 0 4 0 > dt/axim/ranges
bus /axim/pcie@1000120000 3 2
printf 'pci\0' > dt/axim/pcie@1000120000/device_type
cell 0x02000000 0 0 0x1f 0 0 0xfffffffc 0x43000000 4 0 0x1c 0 3 0 > dt/axim/pcie@1000120000/ranges
bus /axim/pcie@1000120000/pci@0,0 3 2
printf 'pci\0' > dt/axim/pcie@1000120000/pci@0,0/device_type
: > dt/axim/pcie@1000120000/pci@0,0/ranges
bus /axim/pcie@1000120000/pci@0,0/dev@0,0 3 2
cell 1 0 0 0x82010000 0 0 0 0x400000 > dt/axim/pcie@1000120000/pci@0,0/dev@0,0/ranges
bus /axim/pcie@1000120000/pci@0,0/dev@0,0/pci-ep-bus@1 2 2
cell 0 0x40000000 1 0 0 0 0x400000 > dt/axim/pcie@1000120000/pci@0,0/dev@0,0/pci-ep-bus@1/ranges
controller /axim/pcie@1000120000/pci@0,0/dev@0,0/pci-ep-bus@1/gpio@d0000 0x60 raspberrypi,rp1-gpio \
	0 0x400d0000 0 0xc000 0 0x400e0000 0 0xc000 0 0x400f0000 0 0xc000
controller /soc5/gpio@7d508500 0x40 brcm,brcmstb-gpio 0 0x7d508500 0 0x40
bus /noranges 1 1
controller /noranges/gpio@1000 0x41 rockchip,gpio-bank 0x1000 0x100
controller /pinctrl/gpio@fe770000 0x42 rockchip,gpio-bank 0 0xfe770000 0 0x100
cell 3 > dt/pinctrl/gpio@fe770000/#gpio-cells
# A gpiochip whose node has no compatible or phandle, like a gpiolib shared proxy.
mkdir -p sys/bus/gpio/devices/gpiochip$chip/of_node
# pps entry platform-device gpios-cells...: a pps-gpio device; DRIVER names another driver
pps() {
	entry=$1 dev=$2; shift 2
	mkdir -p "sys/class/pps/$entry"
	echo "$dev.-1" > "sys/class/pps/$entry/name"
	if [ ! -e "sys/bus/platform/devices/$dev" ]; then
		mkdir -p "dt/$dev" "sys/bus/platform/devices/$dev"
		cell "$@" > "dt/$dev/gpios"
		ln -s "../../../../../dt/$dev" "sys/bus/platform/devices/$dev/of_node"
		ln -s "../../../bus/platform/drivers/${DRIVER:-pps-gpio}" "sys/bus/platform/devices/$dev/driver"
	fi
}
pps pps0 pps@12 9 18 0
pps pps1 pps@12
pps fakepps pps@12
pps ppslow ppslow 9 18 1
pps ppsbcm ppsbcm 7 18 0
pps ppsbcm54 ppsbcm54 7 54 0
pps pps2711 pps2711 8 18 0
pps ppsrk ppsrk 0x22 18 0
pps ppsrk3 ppsrk3 0x53 3 0
pps ppsrk32 ppsrk32 0x22 32 0
pps ppsk3 ppsk3 0x31 18 0
pps ppsk3big ppsk3big 0x31 73 0
pps ppsmxc ppsmxc 0x36 18 0
pps ppsbad ppsbad 0x40 18 0
pps ppsnone ppsnone 0x99 18 0
pps ppsnoranges ppsnoranges 0x41 18 0
pps ppscells ppscells 0x42 18 0 0
pps ppsmain ppsmain 0x60 18 0
DRIVER=other-driver pps ppsother ppsother 9 18 0
mkdir -p sys/class/pps/ptp
echo ptp0 > sys/class/pps/ptp/name
