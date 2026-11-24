/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The DaVinci/Keystone GPIO controllers of TI K3 SoCs, such as the AM67A of the BeagleY-AI, as
 * the kernel's gpio-davinci driver handles them. SoC-specific compatible strings, such as
 * ti,am64-gpio, are followed by ti,keystone-gpio, and the ti,ngpio property gives the number
 * of lines. */
#include <inttypes.h>
#include <stdio.h>
#include "family.h"

static const struct compatible compatible[] = {
	{ "ti,keystone-gpio", 0 },
	{ NULL, 0 }
};

/* The GPIO_IN_DATAnn registers: one in each register set of 32 lines, the sets 0x28 apart,
 * the first IN_DATA at 0x20. */
static size_t level(unsigned line, uint32_t *mask)
{
	*mask = UINT32_C(1) << line % 32;
	return 0x20 + 0x28 * (line / 32);
}

/* GPIO_PID, at 0, has FUNC 0x483 for the DaVinci/Keystone GPIO. */
static const char *check(const volatile char *regs)
{
	static char why[64];
	uint32_t pid = *(const volatile uint32_t *)regs;
	if ((pid >> 16 & 0xfff) == 0x483) return NULL;
	snprintf(why, sizeof why, "unsupported TI GPIO controller PID 0x%08" PRIx32, pid);
	return why;
}

const struct family k3 = {
	.name = "k3", .compatible = compatible, .lines_property = "ti,ngpio",
	.level = level, .check = check, .extent = 4,
};
