/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 James Clark
 */
/* The interface between ppsbias and a backend, which describes a kind of GPIO controller as
 * the kernel driver that handles it does: which device-tree nodes the backend handles, how many
 * lines a controller has, how the registers are mapped, and which register holds a line's input.
 * A backend is pure: it reads nothing from the system, not even registers, and is given what
 * ppsbias finds there, including the value of its identification register.
 * Adding a backend is one new file, one line in the backends list in ppsbias.c, and one word
 * in BACKENDS in the Makefiles.
 *
 * Not yet handled: a controller whose input register does not show the pin's level while
 * pps-gpio holds the line for its interrupt, such as the Allwinner A733, whose kernel driver
 * sets irq_read_needs_mux. A backend for one needs a way to say so, so that ppsbias refuses
 * to use it. */
#ifndef PPSBIAS_BACKEND_H
#define PPSBIAS_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A device-tree compatible string the backend handles, with the number of lines of a controller
 * whose node has it; lines is 0 when the node's lines_property gives the number instead. */
struct compatible { const char *name; unsigned lines; };

struct backend {
	const char *name;                     /* the name of the kernel's driver, such as "rp1" */
	const struct compatible *compatible;  /* terminated by an entry whose name is NULL */
	const char *lines_property;           /* the node property giving its number of lines, or NULL */
	/* Optional: a gpiomem device that maps the controller's registers from the start of its
	 * first register range, and the length of its window. Without one, or when it is missing,
	 * the registers are mapped through /dev/mem at their CPU physical address. */
	const char *device;
	size_t window;
	/* The index in the node's reg property of the register range holding the registers below. */
	unsigned reg;
	/* The offset in that range of a register identifying the controller's version, or -1. */
	int id;
	/* Where to read line: the offset in that range of its 32-bit input register, and a mask
	 * with the line's bit set. id is the identification register's value, or 0 if there is
	 * none. Returns false if the backend does not support that version of the controller. */
	bool (*input)(uint32_t id, unsigned line, size_t *offset, uint32_t *mask);
};

extern const struct backend bcm2835_backend, rp1_backend, rockchip_backend, k3_backend, mxc_backend;
#endif
