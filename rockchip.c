/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The GPIO controllers of Rockchip SoCs, as the kernel's gpio-rockchip driver handles them: one
 * controller per 32-line bank. All use the same compatible string; as in the driver, the version
 * register tells GPIO v1 (such as the RK3399's) from v2 (RK3566, RK3568, RK3588). */
#include "backend.h"

static const struct compatible compatible[] = {
	{ "rockchip,gpio-bank", 32 },
	{ NULL, 0 }
};

/* GPIO_EXT_PORT: at 0x70 in GPIO v2, whose version register at 0x78 holds one of the IDs the
 * driver knows; at 0x50 in GPIO v1, which reads 0 there. */
static bool input(uint32_t id, unsigned line, size_t *offset, uint32_t *mask)
{
	if (id == UINT32_C(0x01000c2b) || id == UINT32_C(0x0101157c) || id == UINT32_C(0x010219c8))
		*offset = 0x70;
	else if (id == 0)
		*offset = 0x50;
	else
		return false;
	*mask = UINT32_C(1) << line;
	return true;
}

const struct backend rockchip_backend = {
	.name = "rockchip", .compatible = compatible,
	.reg = 0, .id = 0x78, .input = input,
};
