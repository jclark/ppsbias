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

/* GPIO_PSR, the pad status register, which the kernel's driver reads for an input's level. */
static size_t level(unsigned line, uint32_t *mask)
{
	*mask = UINT32_C(1) << line;
	return 0x08;
}

/* The controller has no identification register to check. */
const struct backend mxc_backend = {
	.name = "mxc", .compatible = compatible,
	.level = level,
};
