/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The GPIO controllers of Rockchip SoCs, as the kernel's gpio-rockchip driver handles them:
 * one controller per 32-line bank. Only GPIO v2, as in the RK3566, RK3568 and RK3588, is
 * supported. */
#include <inttypes.h>
#include <stdio.h>
#include "backend.h"

static const struct compatible compatible[] = {
	{ "rockchip,gpio-bank", 32 },
	{ NULL, 0 }
};

/* GPIO_EXT_PORT, which is at 0x70 in GPIO v2. */
static size_t level(unsigned line, uint32_t *mask)
{
	*mask = UINT32_C(1) << line;
	return 0x70;
}

/* GPIO v1 has the level at 0x50 and no version register; GPIO v2 has the version at 0x78. */
static const char *check(const volatile char *regs)
{
	static char why[64];
	uint32_t version = *(const volatile uint32_t *)(regs + 0x78);
	if (version == UINT32_C(0x01000c2b) || version == UINT32_C(0x0101157c) || version == UINT32_C(0x010219c8))
		return NULL;
	snprintf(why, sizeof why, "unsupported Rockchip GPIO controller version 0x%08" PRIx32, version);
	return why;
}

const struct backend rockchip_backend = {
	.name = "rockchip", .compatible = compatible,
	.level = level, .check = check, .extent = 0x7c,
};
