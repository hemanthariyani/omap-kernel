/*
 * gcmain.h
 *
 * Copyright (C) 2010-2011 Vivante Corporation.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef GCMAIN_H
#define GCMAIN_H

#include <linux/gccore.h>

/*
 * Register access.
 */

unsigned int gc_read_reg(unsigned int address);
void gc_write_reg(unsigned int address, unsigned int data);

/*
 * Paged memory allocator.
 */

#define USE_DMA_COHERENT 1

#if USE_DMA_COHERENT
struct gcpage {
	unsigned int size;
	unsigned int physical;
	unsigned int *logical;
};
#else
struct gcpage {
	unsigned int order;
	unsigned int size;
	struct page *pages;
	unsigned int physical;
	unsigned int *logical;
};
#endif

enum gcerror gc_alloc_pages(struct gcpage *gcpage, unsigned int size);
void gc_free_pages(struct gcpage *p);

#endif
