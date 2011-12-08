/*
 * gccmdbuf.c
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/dma-mapping.h>

#include "gcreg.h"
#include "gcmain.h"
#include "gccmdbuf.h"

#define GC_ENABLE_GPU_COUNTERS	1

#ifndef GC_DUMP
#	define GC_DUMP 0
#endif

#if GC_DUMP
#	define GC_PRINT printk
#else
#	define GC_PRINT(...)
#endif

struct cmdbuf {
	struct gcpage page;

	int mapped;
	u32 mapped_physical;

	u32 *logical;
	u32 physical;

	u32 available;
	u32 data_size;
};

static struct cmdbuf cmdbuf;

enum gcerror cmdbuf_init(void)
{
	enum gcerror gcerror;

	gcerror = gc_alloc_pages(&cmdbuf.page, PAGE_SIZE);
	if (gcerror != GCERR_NONE)
		return GCERR_SETGRP(gcerror, GCERR_CMD_ALLOC);

	memset(cmdbuf.page.logical, 0x0, cmdbuf.page.size);

	cmdbuf.mapped = false;
	cmdbuf.logical = cmdbuf.page.logical;
	cmdbuf.physical = cmdbuf.page.physical;

	cmdbuf.available = PAGE_SIZE;
	cmdbuf.data_size = 0;

	GC_PRINT(KERN_INFO "%s(%d): Initialized command buffer.\n",
		__func__, __LINE__);

	GC_PRINT(KERN_INFO "%s(%d):   physical = 0x%08X\n",
		__func__, __LINE__, cmdbuf.page.physical);

	GC_PRINT(KERN_INFO "%s(%d):   logical = 0x%08X\n",
		__func__, __LINE__, (u32) cmdbuf.page.logical);

	GC_PRINT(KERN_INFO "%s(%d):   size = %d\n",
		__func__, __LINE__, cmdbuf.page.size);

	return GCERR_NONE;
}

enum gcerror cmdbuf_map(struct mmu2dcontext *ctxt)
{
	enum gcerror gcerror;
	struct mmu2dphysmem mem;
	struct mmu2darena *mapped;

	mem.base = (u32) cmdbuf.page.logical;
	mem.offset = 0;
	mem.count = 1;
	mem.pages = (pte_t *) &cmdbuf.page.physical;
	mem.pagesize = PAGE_SIZE;

	gcerror = mmu2d_map(ctxt, &mem, &mapped);
	if (gcerror != 0)
		return gcerror;

	if (cmdbuf.mapped) {
		if (mapped->address != cmdbuf.mapped_physical) {
			GC_PRINT(KERN_WARNING
				"%s(%d): inconsitent command buffer mapping!\n",
				__func__, __LINE__);
		}
	} else {
		cmdbuf.mapped = true;
	}

	cmdbuf.mapped_physical = mapped->address;
	cmdbuf.physical        = mapped->address + cmdbuf.data_size;

	GC_PRINT(KERN_INFO "%s(%d): Mapped command buffer.\n",
		__func__, __LINE__);

	GC_PRINT(KERN_INFO "%s(%d):   physical = 0x%08X (mapped)\n",
		__func__, __LINE__, cmdbuf.mapped_physical);

	GC_PRINT(KERN_INFO "%s(%d):   logical = 0x%08X\n",
		__func__, __LINE__, (u32) cmdbuf.page.logical);

	GC_PRINT(KERN_INFO "%s(%d):   size = %d\n",
		__func__, __LINE__, cmdbuf.page.size);

	return GCERR_NONE;
}

enum gcerror cmdbuf_alloc(u32 size, u32 **logical, u32 *physical)
{
	if ((cmdbuf.logical == NULL) || (size > cmdbuf.available))
		return GCERR_CMD_ALLOC;

	size = (size + 3) & ~3;

	*logical  = cmdbuf.logical;
	*physical = cmdbuf.physical;

	cmdbuf.logical   += (size >> 2);
	cmdbuf.physical  += size;
	cmdbuf.available -= size;
	cmdbuf.data_size += size;

	return GCERR_NONE;
}

int cmdbuf_flush(u32 *logical)
{
	static const int flushSize = 4 * sizeof(u32);

	if (logical != NULL) {
		u32 base;
		u32 count;

#if ENABLE_POLLING
		u32 retry;
#endif

		/* Append EVENT(Event, destination). */
		logical[0]
			= LS(AQEventRegAddrs, 1);

		logical[1]
			= SETFIELDVAL(0, AQ_EVENT, PE_SRC, ENABLE)
			| SETFIELD(0, AQ_EVENT, EVENT_ID, 16);

		/* Stop FE. */
		logical[2]
			= SETFIELDVAL(0, AQ_COMMAND_END_COMMAND, OPCODE, END);

#if GC_DUMP
		/* Dump command buffer. */
		cmdbuf_dump();
#endif

		/* Determine the command buffer base address. */
		base = cmdbuf.mapped
			? cmdbuf.mapped_physical : cmdbuf.page.physical;

		/* Compute the data count. */
		count = (cmdbuf.data_size + 7) >> 3;

#if ENABLE_POLLING
		int_data = 0;
#endif

		GC_PRINT("starting DMA at 0x%08X with count of %d\n",
			base, count);

#if USE_DMA_COHERENT
		dma_sync_single_for_device(NULL, cmdbuf.page.physical,
						cmdbuf.page.size,
						DMA_TO_DEVICE);
#endif

#if GC_DUMP || GC_ENABLE_GPU_COUNTERS
		/* Reset hardware counters. */
		gc_write_reg(GC_RESET_MEM_COUNTERS_Address, 1);
#endif

		/* Enable all events. */
		gc_write_reg(AQ_INTR_ENBL_Address, ~0U);

		/* Write address register. */
		gc_write_reg(AQ_CMD_BUFFER_ADDR_Address, base);

		/* Write control register. */
		gc_write_reg(AQ_CMD_BUFFER_CTRL_Address,
			SETFIELDVAL(0, AQ_CMD_BUFFER_CTRL, ENABLE, ENABLE) |
			SETFIELD(0, AQ_CMD_BUFFER_CTRL, PREFETCH, count)
			);

		/* Wait for the interrupt. */
#if ENABLE_POLLING
		retry = 0;
		while (1) {
			if (int_data != 0)
				break;

			msleep(500);
			retry += 1;

			if ((retry % 5) == 0)
				gpu_status((char *) __func__, __LINE__, 0);
		}
#else
		wait_event_interruptible(gc_event, done == true);
#endif

#if GC_DUMP
		gpu_status((char *) __func__, __LINE__, 0);
#endif

		/* Reset the buffer. */
		cmdbuf.logical  = cmdbuf.page.logical;
		cmdbuf.physical = base;

		cmdbuf.available = cmdbuf.page.size;
		cmdbuf.data_size = 0;
	}

	return flushSize;
}

void gpu_id(void)
{
	u32 chipModel;
	u32 chipRevision;
	u32 chipDate;
	u32 chipTime;
	u32 chipFeatures;
	u32 chipMinorFeatures;

	chipModel = gc_read_reg(GC_CHIP_ID_Address);
	chipRevision = gc_read_reg(GC_CHIP_REV_Address);
	chipDate = gc_read_reg(GC_CHIP_DATE_Address);
	chipTime = gc_read_reg(GC_CHIP_TIME_Address);
	chipFeatures = gc_read_reg(GC_FEATURES_Address);
	chipMinorFeatures = gc_read_reg(GC_MINOR_FEATURES0_Address);

	GC_PRINT(KERN_INFO "CHIP IDENTITY\n");
	GC_PRINT(KERN_INFO "  model=%X\n", chipModel);
	GC_PRINT(KERN_INFO "  revision=%X\n", chipRevision);
	GC_PRINT(KERN_INFO "  date=%X\n", chipDate);
	GC_PRINT(KERN_INFO "  time=%X\n", chipTime);
	GC_PRINT(KERN_INFO "  chipFeatures=0x%08X\n", chipFeatures);
}

void gpu_status(char *function, int line, u32 acknowledge)
{
	int i;
	u32 idle;
	u32 dma_state, dma_addr;
	u32 dma_low_data, dma_high_data;
	u32 status;
	u32 mmu;
	u32 address;
	u32 total_reads;
	u32 total_writes;
	u32 total_read_bursts;
	u32 total_write_bursts;
	u32 total_read_reqs;
	u32 total_write_reqs;

	GC_PRINT(KERN_INFO "%s(%d): Current GPU status.\n",
		function, line);

	idle = gc_read_reg(AQ_HI_IDLE_Address);
	GC_PRINT(KERN_INFO "%s(%d):   idle = 0x%08X\n",
		function, line, idle);

	dma_state = gc_read_reg(AQFE_DEBUG_STATE_Address);
	GC_PRINT(KERN_INFO "%s(%d):   DMA state = 0x%08X\n",
		function, line, dma_state);

	dma_addr = gc_read_reg(AQFE_DEBUG_CUR_CMD_ADR_Address);
	GC_PRINT(KERN_INFO "%s(%d):   DMA address = 0x%08X\n",
		function, line, dma_addr);

	dma_low_data = gc_read_reg(AQFE_DEBUG_CMD_LOW_REG_Address);
	GC_PRINT(KERN_INFO "%s(%d):   DMA low data = 0x%08X\n",
		function, line, dma_low_data);

	dma_high_data = gc_read_reg(AQFE_DEBUG_CMD_HI_REG_Address);
	GC_PRINT(KERN_INFO "%s(%d):   DMA high data = 0x%08X\n",
		function, line, dma_high_data);

	total_reads = gc_read_reg(GC_TOTAL_READS_Address);
	GC_PRINT(KERN_INFO "%s(%d):   Total memory reads = %d\n",
		function, line, total_reads);

	total_writes = gc_read_reg(GC_TOTAL_WRITES_Address);
	GC_PRINT(KERN_INFO "%s(%d):   Total memory writes = %d\n",
		function, line, total_writes);

	total_read_bursts = gc_read_reg(GC_TOTAL_READ_BURSTS_Address);
	GC_PRINT(KERN_INFO "%s(%d):   Total memory read 64-bit bursts = %d\n",
		function, line, total_read_bursts);

	total_write_bursts = gc_read_reg(GC_TOTAL_WRITE_BURSTS_Address);
	GC_PRINT(KERN_INFO "%s(%d):   Total memory write 64-bit bursts = %d\n",
		function, line, total_write_bursts);

	total_read_reqs = gc_read_reg(GC_TOTAL_READ_REQS_Address);
	GC_PRINT(KERN_INFO "%s(%d):   Total memory read requests = %d\n",
		function, line, total_read_reqs);

	total_write_reqs = gc_read_reg(GC_TOTAL_WRITE_REQS_Address);
	GC_PRINT(KERN_INFO "%s(%d):   Total memory write requests = %d\n",
		function, line, total_write_reqs);

	GC_PRINT(KERN_INFO "%s(%d):   interrupt acknowledge = 0x%08X\n",
		function, line, acknowledge);

	if (acknowledge & 0x80000000) {
		GC_PRINT(KERN_INFO "%s(%d):   *** BUS ERROR ***\n",
			function, line);
	}

	if (acknowledge & 0x40000000) {
		u32 mtlb, stlb, offset;

		GC_PRINT(KERN_INFO "%s(%d):   *** MMU ERROR ***\n",
			function, line);

		status = gc_read_reg(GCREG_MMU_STATUS_Address);
		GC_PRINT(KERN_INFO "%s(%d):   MMU status = 0x%08X\n",
			function, line, status);

		for (i = 0; i < 4; i += 1) {
			mmu = status & 0xF;
			status >>= 4;

			if (mmu == 0)
				continue;

			switch (mmu) {
			case 1:
				GC_PRINT(KERN_INFO
					"%s(%d):   MMU%d: slave not present\n",
					function, line, i);
				break;

			case 2:
				GC_PRINT(KERN_INFO
					"%s(%d):   MMU%d: page not present\n",
					function, line, i);
				break;

			case 3:
				GC_PRINT(KERN_INFO
					"%s(%d):   MMU%d: write violation\n",
					function, line, i);
				break;

			default:
				GC_PRINT(KERN_INFO
					"%s(%d):   MMU%d: unknown state\n",
					function, line, i);
			}

			address = gc_read_reg(GCREG_MMU_EXCEPTION_Address + i);

			mtlb   = (address & MMU_MTLB_MASK) >> MMU_MTLB_SHIFT;
			stlb   = (address & MMU_STLB_MASK) >> MMU_STLB_SHIFT;
			offset =  address & MMU_OFFSET_MASK;

			GC_PRINT(KERN_INFO
				"%s(%d):   MMU%d: exception address = 0x%08X\n",
				function, line, i, address);

			GC_PRINT(KERN_INFO
				"%s(%d):            MTLB entry = %d\n",
				function, line, mtlb);

			GC_PRINT(KERN_INFO
				"%s(%d):            STLB entry = %d\n",
				function, line, stlb);

			GC_PRINT(KERN_INFO
				"%s(%d):            Offset = 0x%08X (%d)\n",
				function, line, offset, offset);
		}
	}
}

void cmdbuf_dump(void)
{
	u32 i, count, base;

	base = cmdbuf.mapped ? cmdbuf.mapped_physical : cmdbuf.page.physical;

	GC_PRINT(KERN_INFO "%s(%d): Current command buffer.\n",
		__func__, __LINE__);

	GC_PRINT(KERN_INFO "%s(%d):   physical = 0x%08X%s\n",
		__func__, __LINE__, base, cmdbuf.mapped ? " (mapped)" : "");

	GC_PRINT(KERN_INFO "%s(%d):   logical = 0x%08X\n",
		__func__, __LINE__, (u32) cmdbuf.page.logical);

	GC_PRINT(KERN_INFO "%s(%d):   current data size = %d\n",
		__func__, __LINE__, cmdbuf.data_size);

	GC_PRINT(KERN_INFO "%s(%d)\n",
		__func__, __LINE__);

	count = cmdbuf.data_size / 4;

	for (i = 0; i < count; i += 1) {
		GC_PRINT(KERN_INFO "%s(%d):   [0x%08X]: 0x%08X\n",
			__func__, __LINE__,
			base + i * 4,
			cmdbuf.page.logical[i]
			);
	}
}
