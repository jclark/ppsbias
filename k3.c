/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The DaVinci/Keystone GPIO controllers of TI K3 SoCs, such as the AM67A of the BeagleY-AI, as
 * the kernel's gpio-davinci driver handles them. SoC-specific compatible strings, such as
 * ti,am64-gpio, are followed by ti,keystone-gpio, and the ti,ngpio property gives the number
 * of lines. */
#include "backend.h"

static const struct compatible compatible[] = {
	{ "ti,keystone-gpio", 0 },
	{ NULL, 0 }
};

/* The GPIO_IN_DATAnn registers: one in each register set of 32 lines, the sets 0x28 apart, the
 * first IN_DATA at 0x20. GPIO_PID, at 0, has FUNC 0x483 for the DaVinci/Keystone GPIO with this
 * layout; the driver does not check it, but a different FUNC means a different controller. */
static bool input(uint32_t id, unsigned line, size_t *offset, uint32_t *mask)
{
	if ((id >> 16 & 0xfff) != 0x483) return false;
	*offset = 0x20 + 0x28 * (line / 32);
	*mask = UINT32_C(1) << line % 32;
	return true;
}

const struct backend k3_backend = {
	.name = "k3", .compatible = compatible, .lines_property = "ti,ngpio",
	.reg = 0, .id = 0, .input = input,
};
