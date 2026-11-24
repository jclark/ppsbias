/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The GPIO controllers of NXP i.MX SoCs from the i.MX35 on, as the kernel's gpio-mxc driver
 * handles them: one controller per 32-line port. SoC-specific compatible strings, such as
 * fsl,imx8mp-gpio on the i.MX 8M Plus, are followed by fsl,imx35-gpio. */
#include "backend.h"

static const struct compatible compatible[] = {
	{ "fsl,imx35-gpio", 32 },
	{ NULL, 0 }
};

/* GPIO_PSR, the pad status register, which the kernel's driver reads for an input's level.
 * The controller has no identification register. */
static bool input(uint32_t id, unsigned line, size_t *offset, uint32_t *mask)
{
	(void)id;
	*offset = 0x08;
	*mask = UINT32_C(1) << line;
	return true;
}

const struct backend mxc_backend = {
	.name = "mxc", .compatible = compatible,
	.reg = 0, .id = -1, .input = input,
};
