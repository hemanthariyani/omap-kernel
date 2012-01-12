/*
 * gcmain.c
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

#include <linux/version.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <plat/cpu.h>

#include "gcmain.h"
#include "gccmdbuf.h"
#include "gcmmu.h"
#include "gcreg.h"

#ifndef GC_DUMP
#	define GC_DUMP 0
#endif

#if GC_DUMP
#	define GC_PRINT printk
#else
#	define GC_PRINT(...)
#endif

#define GC_DETECT_TIMEOUT 0

#define GC_DEVICE "gc-core"

#define DEVICE_INT	(32 + 125)
#define DEVICE_REG_BASE	0x59000000
#define DEVICE_REG_SIZE	(256 * 1024)

static dev_t dev;
static struct cdev cd;
static struct device *device;
static struct class *class;

struct gccore {
	void *priv;
};

static struct gccore gcdevice;

static int irqline = 48;
module_param(irqline, int, 0644);

static long registerMemBase = 0xF1840000;
module_param(registerMemBase, long, 0644);

static LIST_HEAD(mmu_list);

struct clientinfo {
	struct list_head head;
	struct mmu2dcontext ctxt;
	u32 pid;
	struct task_struct *task;
	int mmu_dirty;
};

/*******************************************************************************
 * Register access.
 */

static void *g_reg_base;

unsigned int gc_read_reg(unsigned int address)
{
	return readl((unsigned char *) g_reg_base + address);
}

void gc_write_reg(unsigned int address, unsigned int data)
{
	writel(data, (unsigned char *) g_reg_base + address);
}


/*******************************************************************************
 * Page allocation routines.
 */

enum gcerror gc_alloc_pages(struct gcpage *p, unsigned int size)
{
	enum gcerror gcerror;
	void *logical;
	int order, count;

	p->pages = NULL;
	p->logical = NULL;
	p->physical = ~0UL;

	order = get_order(size);

	p->order = order;
	p->size = (1 << order) * PAGE_SIZE;

	GC_PRINT(KERN_ERR "%s(%d): requested size=%d\n",
		__func__, __LINE__, size);

	GC_PRINT(KERN_ERR "%s(%d): rounded up size=%d\n",
		__func__, __LINE__, p->size);

	GC_PRINT(KERN_ERR "%s(%d): order=%d\n",
		__func__, __LINE__, order);

	p->pages = alloc_pages(GFP_KERNEL, order);
	if (p->pages == NULL) {
		gcerror = GCERR_OOPM;
		goto fail;
	}

	p->physical = page_to_phys(p->pages);
	p->logical = (unsigned int *) page_address(p->pages);

	if (p->logical == NULL) {
		gcerror = GCERR_PMMAP;
		goto fail;
	}

	/* Reserve pages. */
	logical = p->logical;
	count = p->size / PAGE_SIZE;

	while (count) {
		SetPageReserved(virt_to_page(logical));

		logical = (unsigned char *) logical + PAGE_SIZE;
		count  -= 1;
	}

	GC_PRINT(KERN_ERR "%s(%d): (0x%08X) pages=0x%08X,"
				" logical=0x%08X, physical=0x%08X, size=%d\n",
				__func__, __LINE__,
				(unsigned int) p,
				(unsigned int) p->pages,
				(unsigned int) p->logical,
				(unsigned int) p->physical,
				p->size);

	return GCERR_NONE;

fail:
	gc_free_pages(p);
	return gcerror;
}

void gc_free_pages(struct gcpage *p)
{
	GC_PRINT(KERN_ERR "%s(%d): (0x%08X) pages=0x%08X,"
				" logical=0x%08X, physical=0x%08X, size=%d\n",
				__func__, __LINE__,
				(unsigned int) p,
				(unsigned int) p->pages,
				(unsigned int) p->logical,
				(unsigned int) p->physical,
				p->size);

	if (p->logical != NULL) {
		void *logical;
		int count;

		logical = p->logical;
		count = p->size / PAGE_SIZE;

		while (count) {
			ClearPageReserved(virt_to_page(logical));

			logical = (unsigned char *) logical + PAGE_SIZE;
			count  -= 1;
		}

		p->logical = NULL;
	}

	if (p->pages != NULL) {
		__free_pages(p->pages, p->order);
		p->pages = NULL;
	}

	p->physical = ~0UL;
	p->order = 0;
	p->size = 0;
}

void gc_flush_pages(struct gcpage *p)
{
	GC_PRINT(KERN_ERR "%s(%d): (0x%08X) pages=0x%08X,"
				" logical=0x%08X, physical=0x%08X, size=%d\n",
				__func__, __LINE__,
				(unsigned int) p,
				(unsigned int) p->pages,
				(unsigned int) p->logical,
				(unsigned int) p->physical,
				p->size);

	dmac_flush_range(p->logical, (unsigned char *) p->logical + p->size);
	outer_flush_range(p->physical, p->physical + p->size);
}

/*******************************************************************************
 * Interrupt handling.
 */

#if ENABLE_POLLING
struct completion g_gccoreint;
static u32 g_gccoredata;

void gc_wait_interrupt(void)
{
	might_sleep();

	spin_lock_irq(&g_gccoreint.wait.lock);

	if (g_gccoreint.done) {
		g_gccoreint.done = 0;
	} else {
#if GC_DETECT_TIMEOUT
		int timeout = 10 * HZ;
#else
		int timeout = MAX_SCHEDULE_TIMEOUT;
#endif

		DECLARE_WAITQUEUE(wait, current);
		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_tail(&g_gccoreint.wait, &wait);

		while (1) {
			if (signal_pending(current)) {
				/* Interrupt received. */
				break;
			}

			__set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&g_gccoreint.wait.lock);
			timeout = schedule_timeout(timeout);
			spin_lock_irq(&g_gccoreint.wait.lock);

			if (g_gccoreint.done) {
				g_gccoreint.done = 0;
				break;
			}

			gpu_status((char *) __func__, __LINE__, 0);
			timeout = 10 * HZ;
		}

		__remove_wait_queue(&g_gccoreint.wait, &wait);
	}

	spin_unlock_irq(&g_gccoreint.wait.lock);
}

u32 gc_get_interrupt_data(void)
{
	u32 data;

	data = g_gccoredata;
	g_gccoredata = 0;

	return data;
}
#endif

static struct workqueue_struct *gcwq;
DECLARE_WAIT_QUEUE_HEAD(gc_event);
int done;

static void gc_work(struct work_struct *ignored)
{
	done = true;
	wake_up_interruptible(&gc_event);
}
static DECLARE_WORK(gcwork, gc_work);

static irqreturn_t gc_irq(int irq, void *p)
{
	u32 data = 0;

#if !ENABLE_POLLING
	struct clientinfo *client = NULL;
#endif

	/* Read gcregIntrAcknowledge register. */
	data = gc_read_reg(GCREG_INTR_ACKNOWLEDGE_Address);

	GC_PRINT(KERN_ERR "%s(%d): data=0x%08X\n", __func__, __LINE__, data);

	/* Our interrupt? */
	if (data != 0) {
#if GC_DUMP
		/* Dump GPU status. */
		gpu_status((char *) __func__, __LINE__, data);
#endif

#if ENABLE_POLLING
		g_gccoredata = data & 0x3FFFFFFF;
		complete(&g_gccoreint);
#else
		/* TODO: we need to wait for an interrupt after enabling
			 the mmu, but we don't want to send a signal to
			 the user space.  Instead, we want to send a signal
			 to the driver.
		*/

		if (data == 0x10000) {
			queue_work(gcwq, &gcwork);
		} else {
			client = (struct clientinfo *)
					((struct gccore *)p)->priv;
			if (client != NULL)
				send_sig(SIGUSR1, client->task, 0);
		}
#endif
	}

	return IRQ_HANDLED;
}

/*******************************************************************************
 * Internal routines.
 */

static enum gcerror find_client(struct clientinfo **client)
{
	enum gcerror gcerror;
	struct clientinfo *ci = NULL;
	int found = false;

	/* TODO: find the proc ID */
	list_for_each_entry(ci, &mmu_list, head) {
		if (ci->pid == current->tgid) {
			found = true;
			break;
		}
	}

	if (!found) {
		ci = kzalloc(sizeof(struct clientinfo), GFP_KERNEL);
		if (ci == NULL) {
			gcerror = GCERR_SETGRP(GCERR_OODM, GCERR_MMU_CLIENT);
			goto fail;
		}

		memset(ci, 0, sizeof(struct clientinfo));
		INIT_LIST_HEAD(&ci->head);
		ci->pid = current->tgid;
		ci->task = current;

		gcerror = mmu2d_create_context(&ci->ctxt);
		if (gcerror != GCERR_NONE)
			goto fail;

		gcerror = cmdbuf_map(&ci->ctxt);
		if (gcerror != GCERR_NONE)
			goto fail;

		ci->mmu_dirty = true;

		list_add(&ci->head, &mmu_list);
	}

	gcdevice.priv = (void *) ci;
	*client = ci;

	return GCERR_NONE;

fail:
	if (ci != NULL) {
		if (ci->ctxt.mmu != NULL)
			mmu2d_destroy_context(&ci->ctxt);

		kfree(ci);
	}

	return gcerror;
}

/*******************************************************************************
 * API/IOCTL functions.
 */

int gc_commit(struct gccommit *gccommit)
{
	int ret = 0;
	struct gccommit _gccommit;
	struct clientinfo *client = NULL;
	unsigned int cmdflushsize;
	unsigned int mmuflushsize;
	struct gcbuffer *buffer;
	struct gcbuffer _buffer;
	unsigned int buffersize;
	unsigned int allocsize;
	unsigned int *logical;
	unsigned int address;
	struct gcmopipesel *gcmopipesel;

	/* Get gccommit input. */
	if (copy_from_user(&_gccommit, gccommit, sizeof(struct gccommit))) {
		_gccommit.gcerror = GCERR_USER_READ;
		goto exit;
	}

	/* Locate the client entry. */
	_gccommit.gcerror = find_client(&client);
	if (_gccommit.gcerror != GCERR_NONE)
		goto exit;

	/* Set 2D pipe. */
	_gccommit.gcerror = cmdbuf_alloc(sizeof(struct gcmopipesel),
					(void **) &gcmopipesel, NULL);
	if (_gccommit.gcerror != GCERR_NONE)
		return _gccommit.gcerror;

	gcmopipesel->pipesel_ldst = gcmopipesel_pipesel_ldst;
	gcmopipesel->pipesel.reg = gcregpipeselect_2D;

	/* Set the client's master table. */
	_gccommit.gcerror = mmu2d_set_master(&client->ctxt);
	if (_gccommit.gcerror != GCERR_NONE)
		goto exit;

	/* Determine command buffer flush size. */
	cmdflushsize = cmdbuf_flush(NULL);

	/* Go through all buffers one at a time. */
	buffer = _gccommit.buffer;
	while (buffer != NULL) {

		/* Get gcbuffer structure. */
		if (copy_from_user(&_buffer, buffer, sizeof(struct gcbuffer))) {
			_gccommit.gcerror = GCERR_USER_READ;
			goto exit;
		}
		buffer = &_buffer;

		/* Compute the size of the command buffer. */
		buffersize
			= (unsigned char *) buffer->tail
			- (unsigned char *) buffer->head;

		/* Determine MMU flush size. */
		mmuflushsize = client->mmu_dirty ? mmu2d_flush(NULL, 0, 0) : 0;

		/* Reserve command buffer space. */
		allocsize = mmuflushsize + buffersize + cmdflushsize;
		_gccommit.gcerror = cmdbuf_alloc(allocsize,
						(void **) &logical, &address);
		if (_gccommit.gcerror != GCERR_NONE)
			goto exit;

		/* Append MMU flush. */
		if (client->mmu_dirty) {
			mmu2d_flush(logical, address, allocsize);

			/* Skip MMU flush. */
			logical = (unsigned int *)
				((unsigned char *) logical + mmuflushsize);

			/* Validate MMU state. */
			client->mmu_dirty = false;
		}

		/* Copy command buffer. */
		if (copy_from_user(logical, buffer->head, buffersize)) {
			_gccommit.gcerror = GCERR_USER_READ;
			goto exit;
		}

		/* Process fixups. */
		_gccommit.gcerror = mmu2d_fixup(buffer->fixuphead, logical);
		if (_gccommit.gcerror != GCERR_NONE)
			goto exit;

		/* Skip the command buffer. */
		logical = (unsigned int *)
			((unsigned char *) logical + buffersize);

		/* Execute the current command buffer. */
		cmdbuf_flush(logical);

		/* Get the next buffer. */
		buffer = buffer->next;
	}

exit:
	if (copy_to_user(gccommit, &_gccommit, sizeof(struct gccommit))) {
		GC_PRINT(KERN_ERR "%s(%d): transfer failed.\n",
				__func__, __LINE__);
		ret = -EFAULT;
	}

	return ret;
}

int gc_map(struct gcmap *gcmap)
{
	int ret = 0;
	struct gcmap _gcmap;
	struct clientinfo *client = NULL;
	struct mmu2dphysmem mem;
	struct mmu2darena *mapped = NULL;

	/* Get gcmap input. */
	if (copy_from_user(&_gcmap, gcmap, sizeof(struct gcmap))) {
		_gcmap.gcerror = GCERR_USER_READ;
		goto exit;
	}

	/* Locate the client entry. */
	_gcmap.gcerror = find_client(&client);
	if (_gcmap.gcerror != GCERR_NONE)
		goto exit;

	GC_PRINT(KERN_ERR "%s(%d): map client buffer\n",
			__func__, __LINE__);
	GC_PRINT(KERN_ERR "%s(%d):   logical = 0x%08X\n",
			__func__, __LINE__, (unsigned int) _gcmap.logical);
	GC_PRINT(KERN_ERR "%s(%d):   size = %d\n",
			__func__, __LINE__, _gcmap.size);

	/* Initialize the mapping parameters. */
	mem.base = ((u32) _gcmap.logical) & ~(PAGE_SIZE - 1);
	mem.offset = ((u32) _gcmap.logical) & (PAGE_SIZE - 1);
	mem.count = DIV_ROUND_UP(_gcmap.size + mem.offset, PAGE_SIZE);
	mem.pages = NULL;
	mem.pagesize = PAGE_SIZE;

	/* Map the buffer. */
	_gcmap.gcerror = mmu2d_map(&client->ctxt, &mem, &mapped);
	if (_gcmap.gcerror != GCERR_NONE)
		goto exit;

	/* Invalidate the MMU. */
	client->mmu_dirty = true;

	_gcmap.handle = (unsigned int) mapped;

	GC_PRINT(KERN_ERR "%s(%d):   mapped address = 0x%08X\n",
			__func__, __LINE__, mapped->address);
	GC_PRINT(KERN_ERR "%s(%d):   handle = 0x%08X\n",
			__func__, __LINE__, (unsigned int) mapped);

exit:
	if (copy_to_user(gcmap, &_gcmap, sizeof(struct gcmap))) {
		GC_PRINT(KERN_ERR "%s(%d): transfer failed.\n",
				__func__, __LINE__);
		_gcmap.gcerror = GCERR_USER_WRITE;
		ret = -EFAULT;
	}

	if (_gcmap.gcerror != GCERR_NONE) {
		if (mapped != NULL)
			mmu2d_unmap(&client->ctxt, mapped);
	}

	return ret;
}

int gc_unmap(struct gcmap *gcmap)
{
	int ret = 0;
	struct gcmap _gcmap;
	struct clientinfo *client = NULL;

	/* Get gcmap input. */
	if (copy_from_user(&_gcmap, gcmap, sizeof(struct gcmap))) {
		_gcmap.gcerror = GCERR_USER_READ;
		goto exit;
	}

	GC_PRINT(KERN_ERR "%s(%d): unmap client buffer\n",
			__func__, __LINE__);
	GC_PRINT(KERN_ERR "%s(%d):   logical = 0x%08X\n",
			__func__, __LINE__, (unsigned int) _gcmap.logical);
	GC_PRINT(KERN_ERR "%s(%d):   size = %d\n",
			__func__, __LINE__, _gcmap.size);
	GC_PRINT(KERN_ERR "%s(%d):   handle = 0x%08X\n",
			__func__, __LINE__, _gcmap.handle);

	/* Locate the client entry. */
	_gcmap.gcerror = find_client(&client);
	if (_gcmap.gcerror != GCERR_NONE)
		goto exit;

	/* Map the buffer. */
	_gcmap.gcerror = mmu2d_unmap(&client->ctxt,
					(struct mmu2darena *) _gcmap.handle);
	if (_gcmap.gcerror != GCERR_NONE)
		goto exit;

	/* Invalidate the MMU. */
	client->mmu_dirty = true;

	/* Invalidate the handle. */
	_gcmap.handle = ~0U;

exit:
	if (copy_to_user(gcmap, &_gcmap, sizeof(struct gcmap))) {
		GC_PRINT(KERN_ERR "%s(%d): transfer failed.\n",
				__func__, __LINE__);
		ret = -EFAULT;
	}

	return ret;
}

static long ioctl(struct file *filp, u32 cmd, unsigned long arg)
{
	int ret;

	switch (cmd) {
	case GCIOCTL_COMMIT:
		ret = gc_commit((struct gccommit *) arg);
		break;

	case GCIOCTL_MAP:
		ret = gc_map((struct gcmap *) arg);
		break;

	case GCIOCTL_UNMAP:
		ret = gc_unmap((struct gcmap *) arg);
		break;

	default:
		ret = -EINVAL;
	}

	return ret;
}

static s32 open(struct inode *ip, struct file *filp)
{
	return 0;
}

static s32 release(struct inode *ip, struct file *filp)
{
	return 0;
}

static const struct file_operations ops = {
	.open    = open,
	.release = release,
	.unlocked_ioctl = ioctl,
};

static struct platform_driver pd = {
	.driver = {
		.owner = THIS_MODULE,
		.name = GC_DEVICE,
	},
	.probe = NULL,
	.shutdown = NULL,
	.remove = NULL,
};

/*******************************************************************************
 * Driver init/shutdown.
 */

#define GC_MINOR 0
#define GC_COUNT 1

static int __init gc_init(void)
{
	int ret = 0;
	u32 clock = 0;
	struct clk *bb2d_clk;
	int rate;

	GC_PRINT(KERN_ERR "%s(%d): ****** %s %s ******\n",
			 __func__, __LINE__, __DATE__, __TIME__);

	if (cpu_is_omap447x()) {
		bb2d_clk = clk_get(NULL, "bb2d_fck");
		if (IS_ERR(bb2d_clk)) {
			GC_PRINT(KERN_ERR "%s(%d): cannot find bb2d_fck.\n",
				 __func__, __LINE__);
			return -EINVAL;
		}

		rate = clk_get_rate(bb2d_clk);
		GC_PRINT(KERN_ERR
			"%s(%d): BB2D clock is %dMHz\n",
			__func__, __LINE__, (rate / 1000000));

		ret = clk_enable(bb2d_clk);
		if (ret < 0) {
			GC_PRINT(KERN_ERR "%s(%d):failed to enable bb2d_fck.\n",
			 __func__, __LINE__);
			return -EINVAL;
		}

		ret = alloc_chrdev_region(&dev, GC_MINOR, GC_COUNT, GC_DEVICE);
		if (ret != 0)
			return ret;

		cdev_init(&cd, &ops);
		cd.owner = THIS_MODULE;
		ret = cdev_add(&cd, dev, 1);
		if (ret)
			goto free_chrdev_region;

		class = class_create(THIS_MODULE, GC_DEVICE);
		if (IS_ERR(class)) {
			ret = PTR_ERR(class);
			goto free_cdev;
		}

		device = device_create(class, NULL, dev, NULL, GC_DEVICE);
		if (IS_ERR(device)) {
			ret = PTR_ERR(device);
			goto free_class;
		}

		ret = platform_driver_register(&pd);
		if (ret)
			goto free_device;

		g_reg_base = ioremap_nocache(DEVICE_REG_BASE, DEVICE_REG_SIZE);
		if (!g_reg_base)
			goto free_plat_reg;

#if ENABLE_POLLING
		init_completion(&g_gccoreint);
#endif

		/* TODO: clean this up in release call, after a blowup */
		/* create interrupt handler */
		ret = request_irq(DEVICE_INT, gc_irq, IRQF_SHARED, "gc-core",
				&gcdevice);
		if (ret)
			goto free_plat_reg;

		gcwq = create_workqueue("gcwq");
		if (!gcwq)
			goto free_reg_mapping;

		/* gcvPOWER_ON */
		clock = SETFIELD(0, GCREG_HI_CLOCK_CONTROL, CLK2D_DIS, 0) |
			SETFIELD(0, GCREG_HI_CLOCK_CONTROL, FSCALE_VAL, 64) |
			SETFIELD(0, GCREG_HI_CLOCK_CONTROL, FSCALE_CMD_LOAD, 1);

		gc_write_reg(GCREG_HI_CLOCK_CONTROL_Address, clock);

		/* Done loading the frequency scaler. */
		clock = SETFIELD(clock, GCREG_HI_CLOCK_CONTROL,
						FSCALE_CMD_LOAD, 0);

		gc_write_reg(GCREG_HI_CLOCK_CONTROL_Address, clock);

#if GC_DUMP
		/* Print GPU ID. */
		gpu_id();
#endif

		/* Initialize the command buffer. */
		if (cmdbuf_init() != GCERR_NONE)
			goto free_workq;
	}

	/* no errors, so exit */
	goto exit;
free_workq:
	destroy_workqueue(gcwq);
free_reg_mapping:
	iounmap(g_reg_base);
free_plat_reg:
	platform_driver_unregister(&pd);
free_device:
	device_destroy(class, MKDEV(MAJOR(dev), 0));
free_class:
	class_destroy(class);
free_cdev:
	cdev_del(&cd);
free_chrdev_region:
	unregister_chrdev_region(dev, 0);
exit:
	return ret;
}

static void __exit gc_exit(void)
{
	destroy_workqueue(gcwq);
	iounmap(g_reg_base);
	platform_driver_unregister(&pd);
	device_destroy(class, MKDEV(MAJOR(dev), 0));
	class_destroy(class);
	cdev_del(&cd);
	unregister_chrdev_region(dev, 0);
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("www.vivantecorp.com");
MODULE_AUTHOR("www.ti.com");
module_init(gc_init);
module_exit(gc_exit);
