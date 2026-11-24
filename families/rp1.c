/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The GPIO controller of the RP1, the I/O chip of the Raspberry Pi 5 and Compute Module 5, as
 * the kernel's pinctrl-rp1 driver handles it. */
#include "family.h"

static const struct compatible compatible[] = {
	{ "raspberrypi,rp1-gpio", 54 },
	{ NULL, 0 }
};

/* The RIO_IN registers, which the kernel's driver reads for a line's level. They are in the
 * controller's second register range, 0x10000 after the first. The lines are in three banks of
 * 28, 6 and 20; each bank's registers are 0x4000 apart, with bank 0's RIO_IN at offset 8. */
static size_t level(unsigned line, uint32_t *mask)
{
	unsigned bank = line < 28 ? 0 : line < 34 ? 1 : 2;
	*mask = UINT32_C(1) << (line - (bank == 0 ? 0 : bank == 1 ? 28 : 34));
	return 0x10008 + bank * 0x4000;
}

/* /dev/gpiomem0 is the Raspberry Pi 5's gpiomem device for the RP1 GPIO controller. */
const struct family rp1 = {
	.name = "rp1", .compatible = compatible,
	.device = "/dev/gpiomem0", .window = 0x30000,
	.level = level,
};
