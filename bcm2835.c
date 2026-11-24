/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The GPIO controller of the Broadcom BCM2835 family, as the kernel's pinctrl-bcm2835 driver
 * handles it: the BCM2837 of the Raspberry Pi 3 and the BCM2711 of the Raspberry Pi 4 and
 * Compute Module 4. The BCM2837 uses the BCM2835's compatible string. */
#include "backend.h"

static const struct compatible compatible[] = {
	{ "brcm,bcm2835-gpio", 54 },
	{ "brcm,bcm2711-gpio", 58 },
	{ NULL, 0 }
};

/* The GPLEVn level registers: 32 lines per register, the first at 0x34. */
static bool input(uint32_t id, unsigned line, size_t *offset, uint32_t *mask)
{
	(void)id;
	*offset = 0x34 + 4 * (line / 32);
	*mask = UINT32_C(1) << line % 32;
	return true;
}

/* /dev/gpiomem is the gpiomem device of the Raspberry Pi kernel for the Pi 3 and 4. */
const struct backend bcm2835_backend = {
	.name = "bcm2835", .compatible = compatible,
	.device = "/dev/gpiomem", .window = 0x1000,
	.reg = 0, .id = -1, .input = input,
};
