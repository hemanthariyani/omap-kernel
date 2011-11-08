/*
 * gc2d.c
 *
 * Copyright (C) 2011, Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <linux/bltsville.h>

static struct bventry *ops;	/* shared methods and variables */

static enum bverror gc2d_map(struct bvbuffdesc *buffdesc)
{
	return 0;
}

static enum bverror gc2d_unmap(struct bvbuffdesc *buffdesc)
{
	return 0;
}

static enum bverror gc2d_blt(struct bvbltparams *bltparams)
{
	return 0;
}

/* initialize shared method pointers and global static variables */
void gc2d_init(struct bventry *bv)
{
	ops = bv;

	ops->bv_map = gc2d_map;
	ops->bv_unmap = gc2d_unmap;
	ops->bv_blt = gc2d_blt;
}
