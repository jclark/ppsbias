/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The interface between ppsbias and the files describing a family of GPIO controllers, as the
 * kernel driver that handles them does: which device-tree nodes the family handles, how many
 * lines a controller has, where a line's level is read, and how the registers are mapped.
 * A family is pure: it reads nothing from the system, and is given what ppsbias finds there.
 * Adding a family is one new file here and one line in the families list in ppsbias.c. */
#ifndef PPSBIAS_FAMILY_H
#define PPSBIAS_FAMILY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A device-tree compatible string the family handles, with the number of lines of a controller
 * whose node has it; lines is 0 when the node's lines_property gives the number instead. */
struct compatible { const char *name; unsigned lines; };

struct family {
	const char *name;                     /* the kernel's name for the family, such as "rp1" */
	const struct compatible *compatible;  /* terminated by an entry whose name is NULL */
	const char *lines_property;           /* the node property giving its number of lines, or NULL */
	/* A gpiomem device that maps the controller's registers from the start of its first register
	 * range, and the length of its window; NULL if there is none, when the registers are mapped
	 * through /dev/mem at the CPU address of the first register range. */
	const char *device;
	size_t window;
	/* The offset from the start of the first register range of the register holding line's
	 * level, and the line's bit in it. */
	size_t (*level)(unsigned line, uint32_t *mask);
	/* Optional: checks the mapped registers, which start at regs, are the layout level assumes,
	 * reading no further than extent bytes; returns NULL, or why not. */
	const char *(*check)(const volatile char *regs);
	size_t extent;
};

extern const struct family bcm2835, rp1, rockchip, k3;
#endif
