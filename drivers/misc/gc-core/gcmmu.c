/*
 * gcmmu.c
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
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/pagemap.h>
#include <linux/sched.h>

#include "gcreg.h"
#include "gcmmu.h"
#include "gccmdbuf.h"

/*
 * Debugging.
 */

#ifndef GC_DUMP
#	define GC_DUMP 0
#endif

#if GC_DUMP
#	define GC_PRINT printk
#else
#	define GC_PRINT(...)
#endif

#if !defined(PFN_DOWN)
#	define PFN_DOWN(x) \
		((x) >> PAGE_SHIFT)
#endif

#if !defined(phys_to_pfn)
#	define phys_to_pfn(phys) \
		(PFN_DOWN(phys))
#endif

#if !defined(phys_to_page)
#	define phys_to_page(paddr) \
		(pfn_to_page(phys_to_pfn(paddr)))
#endif

#define ARENA_PREALLOC_SIZE	MMU_PAGE_SIZE
#define ARENA_PREALLOC_COUNT \
	((ARENA_PREALLOC_SIZE - sizeof(struct mmu2darenablock)) \
		/ sizeof(struct mmu2darena))

typedef u32 (*pfn_get_present) (u32 entry);
typedef void (*pfn_print_entry) (u32 index, u32 entry);

struct mm2dtable {
	char *name;
	u32 entry_count;
	u32 vacant_entry;
	pfn_get_present get_present;
	pfn_print_entry print_entry;
};

static inline struct mmu2dprivate *get_mmu(void)
{
	static struct mmu2dprivate _mmu;
	return &_mmu;
}

static u32 get_mtlb_present(u32 entry)
{
	return entry & MMU_MTLB_PRESENT_MASK;
}

static u32 get_stlb_present(u32 entry)
{
	return entry & MMU_STLB_PRESENT_MASK;
}

static void print_mtlb_entry(u32 index, u32 entry)
{
	GC_PRINT(KERN_ERR
		"  entry[%03d]: 0x%08X (stlb=0x%08X, ps=%d, ex=%d, pr=%d)\n",
			index,
			entry,
			entry & MMU_MTLB_SLAVE_MASK,
			(entry & MMU_MTLB_PAGE_SIZE_MASK) >> 2,
			(entry & MMU_MTLB_EXCEPTION_MASK) >> 1,
			(entry & MMU_MTLB_PRESENT_MASK)
			);
}

static void print_stlb_entry(u32 index, u32 entry)
{
	GC_PRINT(KERN_ERR
		"  entry[%03d]: 0x%08X (user=0x%08X, wr=%d, ex=%d, pr=%d)\n",
			index,
			entry,
			entry & MMU_STLB_ADDRESS_MASK,
			(entry & MMU_STLB_WRITEABLE_MASK) >> 2,
			(entry & MMU_STLB_EXCEPTION_MASK) >> 1,
			(entry & MMU_STLB_PRESENT_MASK)
			);
}

static void mmu2d_dump_table(struct mm2dtable *desc, struct gcpage *table)
{
	int present, vacant, skipped;
	u32 *logical;
	u32 entry;
	u32 i;

	if (table->size == 0) {
		GC_PRINT(KERN_ERR "%s table is not allocated.\n", desc->name);
		return;
	}

	GC_PRINT(KERN_ERR "\n%s table:\n", desc->name);
	GC_PRINT(KERN_ERR "  physical=0x%08X\n", (u32) table->physical);
	GC_PRINT(KERN_ERR "  size=%d\n", table->size);

	vacant = -1;
	logical = table->logical;

	for (i = 0; i < desc->entry_count; i += 1) {
		entry = logical[i];

		present = desc->get_present(entry);

		if (!present && (entry == desc->vacant_entry)) {
			if (vacant == -1)
				vacant = i;
			continue;
		}

		if (vacant != -1) {
			skipped = i - vacant;
			vacant = -1;
			GC_PRINT(KERN_ERR
				"              skipped %d vacant entries\n",
				skipped);
		}

		if (present) {
			desc->print_entry(i, entry);
		} else {
			GC_PRINT(KERN_ERR
				"  entry[%03d]: invalid entry value (0x%08X)\n",
				i, entry);
		}
	}

	if (vacant != -1) {
		skipped = i - vacant;
		vacant = -1;
		GC_PRINT(KERN_ERR "              skipped %d vacant entries\n",
			skipped);
	}
}

/*
 * Arena record management.
 */

static enum gcerror mmu2d_get_arena(struct mmu2dprivate *mmu,
					struct mmu2darena **arena)
{
	int i;
	struct mmu2darenablock *block;
	struct mmu2darena *temp;

	if (mmu->arena_recs == NULL) {
		block = kmalloc(ARENA_PREALLOC_SIZE, GFP_KERNEL);
		if (block == NULL)
			return GCERR_SETGRP(GCERR_OODM, GCERR_MMU_ARENA_ALLOC);

		block->next = mmu->arena_blocks;
		mmu->arena_blocks = block;

		temp = (struct mmu2darena *)(block + 1);
		for (i = 0; i < ARENA_PREALLOC_COUNT; i += 1) {
			temp->next = mmu->arena_recs;
			mmu->arena_recs = temp;
			temp += 1;
		}
	}

	*arena = mmu->arena_recs;
	mmu->arena_recs = mmu->arena_recs->next;

	return GCERR_NONE;
}

static void mmu2d_free_arena(struct mmu2dprivate *mmu, struct mmu2darena *arena)
{
	/* Add back to the available arena list. */
	arena->next = mmu->arena_recs;
	mmu->arena_recs = arena;
}

static int mmu2d_siblings(struct mmu2darena *arena1, struct mmu2darena *arena2)
{
	u32 mtlb_idx, stlb_idx;
	u32 count, available;

	mtlb_idx = arena1->mtlb;
	stlb_idx = arena1->stlb;
	count = arena1->count;

	while (count > 0) {
		available = MMU_STLB_ENTRY_NUM - stlb_idx;

		if (available > count) {
			available = count;
			stlb_idx += count;
		} else {
			mtlb_idx += 1;
			stlb_idx  = 0;
		}

		count -= available;
	}

	return ((mtlb_idx == arena2->mtlb) && (stlb_idx == arena2->stlb));
}

/*
 * Slave table allocation management.
 */

#if MMU_ENABLE
static enum gcerror mmu2d_allocate_slave(struct mmu2dcontext *ctxt,
						struct mmu2dstlb **stlb)
{
	enum gcerror gcerror;
	int i;
	struct mmu2dstlbblock *block;
	struct mmu2dstlb *temp;

	if (ctxt->slave_recs == NULL) {
		block = kmalloc(STLB_PREALLOC_SIZE, GFP_KERNEL);
		if (block == NULL)
			return GCERR_SETGRP(GCERR_OODM, GCERR_MMU_STLB_ALLOC);

		block->next = ctxt->slave_blocks;
		ctxt->slave_blocks = block;

		temp = (struct mmu2dstlb *)(block + 1);
		for (i = 0; i < STLB_PREALLOC_COUNT; i += 1) {
			temp->next = ctxt->slave_recs;
			ctxt->slave_recs = temp;
			temp += 1;
		}
	}

	gcerror = gc_alloc_pages(&ctxt->slave_recs->pages, MMU_STLB_SIZE);
	if (gcerror != GCERR_NONE)
		return GCERR_SETGRP(gcerror, GCERR_MMU_STLB_ALLOC);

	/* Remove from the list of available records. */
	temp = ctxt->slave_recs;
	ctxt->slave_recs = ctxt->slave_recs->next;

	/* Invalidate all entries. */
	for (i = 0; i < MMU_STLB_ENTRY_NUM; i += 1)
		temp->pages.logical[i] = MMU_STLB_ENTRY_VACANT;

	/* Reset allocated entry count. */
	temp->count = 0;

	*stlb = temp;
	return GCERR_NONE;
}

static void mmu2d_free_slave(struct mmu2dcontext *ctxt, struct mmu2dstlb *slave)
{
	gc_free_pages(&slave->pages);
	slave->next = ctxt->slave_recs;
	ctxt->slave_recs = slave;
}
#endif

enum gcerror mmu2d_create_context(struct mmu2dcontext *ctxt)
{
	enum gcerror gcerror;

#if MMU_ENABLE
	int i;
	u32 *buffer;
	u32 physical;
	u32 cmdflushsize;
	u32 size;
#endif

	struct mmu2dprivate *mmu = get_mmu();

	if (ctxt == NULL)
		return GCERR_MMU_CTXT_BAD;

	memset(ctxt, 0, sizeof(struct mmu2dcontext));

#if MMU_ENABLE
	/* Allocate MTLB table. */
	gcerror = gc_alloc_pages(&ctxt->master, MMU_MTLB_SIZE);
	if (gcerror != GCERR_NONE) {
		gcerror = GCERR_SETGRP(gcerror, GCERR_MMU_MTLB_ALLOC);
		goto fail;
	}

	/* Allocate an array of pointers to slave descriptors. */
	ctxt->slave = kmalloc(MMU_MTLB_SIZE, GFP_KERNEL);
	if (ctxt->slave == NULL) {
		gcerror = GCERR_SETGRP(GCERR_OODM, GCERR_MMU_STLBIDX_ALLOC);
		goto fail;
	}
	memset(ctxt->slave, 0, MMU_MTLB_SIZE);

	/* Invalidate all entries. */
	for (i = 0; i < MMU_MTLB_ENTRY_NUM; i += 1)
		ctxt->master.logical[i] = MMU_MTLB_ENTRY_VACANT;

	/* Configure the physical address. */
	ctxt->physical
	= SETFIELD(~0U, GCREG_MMU_CONFIGURATION, ADDRESS,
	  (ctxt->master.physical >> GCREG_MMU_CONFIGURATION_ADDRESS_Start))
	& SETFIELDVAL(~0U, GCREG_MMU_CONFIGURATION, MASK_ADDRESS, ENABLED)
	& SETFIELD(~0U, GCREG_MMU_CONFIGURATION, MODE, MMU_MTLB_MODE)
	& SETFIELDVAL(~0U, GCREG_MMU_CONFIGURATION, MASK_MODE, ENABLED);
#endif

	/* Allocate the first vacant arena. */
	gcerror = mmu2d_get_arena(mmu, &ctxt->vacant);
	if (gcerror != GCERR_NONE)
		goto fail;

	/* Everything is vacant. */
	ctxt->vacant->mtlb  = 0;
	ctxt->vacant->stlb  = 0;
	ctxt->vacant->count = MMU_MTLB_ENTRY_NUM * MMU_STLB_ENTRY_NUM;
	ctxt->vacant->next  = NULL;

	/* Nothing is allocated. */
	ctxt->allocated = NULL;

#if MMU_ENABLE
	if (!mmu->enabled) {
		/* Allocate the safe zone. */
		if (mmu->safezone.size == 0) {
			gcerror = gc_alloc_pages(&mmu->safezone,
							MMU_SAFE_ZONE_SIZE);
			if (gcerror != GCERR_NONE) {
				gcerror = GCERR_SETGRP(gcerror,
							GCERR_MMU_SAFE_ALLOC);
				goto fail;
			}
		}

		/* Initialize safe zone to a value. */
		for (i = 0; i < MMU_SAFE_ZONE_SIZE / sizeof(u32); i += 1)
			mmu->safezone.logical[i] = 0xDEADC0DE;

		/* Determine command buffer flush size. */
		cmdflushsize = cmdbuf_flush(NULL);

		/* Allocate command buffer space. */
		size = 4 * sizeof(u32) + cmdflushsize;
		gcerror = cmdbuf_alloc(size, &buffer, &physical);
		if (gcerror != GCERR_NONE) {
			gcerror = GCERR_SETGRP(gcerror, GCERR_MMU_INIT);
			goto fail;
		}

		/* Once the safe address is programmed, it cannot be changed. */
		*buffer++ = LS(gcregMMUSafeAddressRegAddrs, 1);
		*buffer++ = mmu->safezone.physical;

		/* Progfram master table address. */
		*buffer++ = LS(gcregMMUConfigurationRegAddrs, 1);
		*buffer++ = ctxt->physical;

		/* Execute the current command buffer. */
		cmdbuf_flush(buffer);

		/*
		 * Enable MMU. For security reasons, once it is enabled,
		 * the only way to disable is to reset the system.
		 */
		gc_write_reg(
			GCREG_MMU_CONTROL_Address,
			SETFIELDVAL(0, GCREG_MMU_CONTROL, ENABLE, ENABLE));

		/* Mark as enabled. */
		mmu->enabled = 1;
	}
#endif

	/* Reference MMU. */
	mmu->refcount += 1;
	ctxt->mmu = mmu;

	return GCERR_NONE;

fail:
#if MMU_ENABLE
	gc_free_pages(&ctxt->master);
	if (ctxt->slave != NULL)
		kfree(ctxt->slave);
#endif

	return gcerror;
}

enum gcerror mmu2d_destroy_context(struct mmu2dcontext *ctxt)
{
	int i;

	if ((ctxt == NULL) || (ctxt->mmu == NULL))
		return GCERR_MMU_CTXT_BAD;

	if (ctxt->slave != NULL) {
		for (i = 0; i < MMU_MTLB_ENTRY_NUM; i += 1) {
			if (ctxt->slave[i] != NULL) {
				gc_free_pages(&ctxt->slave[i]->pages);
				ctxt->slave[i] = NULL;
			}
		}
		kfree(ctxt->slave);
		ctxt->slave = NULL;
	}

	while (ctxt->allocated != NULL) {
		mmu2d_free_arena(ctxt->mmu, ctxt->allocated);
		ctxt->allocated = ctxt->allocated->next;
	}

	while (ctxt->vacant != NULL) {
		mmu2d_free_arena(ctxt->mmu, ctxt->vacant);
		ctxt->vacant = ctxt->vacant->next;
	}

	gc_free_pages(&ctxt->master);

	ctxt->mmu->refcount -= 1;
	ctxt->mmu = NULL;

	return GCERR_NONE;
}

enum gcerror mmu2d_set_master(struct mmu2dcontext *ctxt)
{
#if MMU_ENABLE
	enum gcerror gcerror;
	u32 *buffer;
	u32 physical;
#endif

	if ((ctxt == NULL) || (ctxt->mmu == NULL))
		return GCERR_MMU_CTXT_BAD;

#if MMU_ENABLE
	/* Allocate command buffer space. */
	gcerror = cmdbuf_alloc(2 * sizeof(u32), &buffer, &physical);
	if (gcerror != GCERR_NONE)
		return GCERR_SETGRP(gcerror, GCERR_MMU_MTLB_SET);

	/* Progfram master table address. */
	buffer[0]
		= SETFIELDVAL(0, AQ_COMMAND_LOAD_STATE_COMMAND, OPCODE,
			    LOAD_STATE)
		| SETFIELD(0, AQ_COMMAND_LOAD_STATE_COMMAND, ADDRESS,
			    gcregMMUConfigurationRegAddrs)
		| SETFIELD(0, AQ_COMMAND_LOAD_STATE_COMMAND, COUNT,
			    1);

	buffer[1]
		= ctxt->physical;
#endif

	return GCERR_NONE;
}

static enum gcerror virt2phys(u32 logical, pte_t *physical)
{
	pgd_t *pgd;	/* Page Global Directory (PGD). */
	pmd_t *pmd;	/* Page Middle Directory (PMD). */
	pte_t *pte;	/* Page Table Entry (PTE). */

	/* Get the pointer to the entry in PGD for the address. */
	pgd = pgd_offset(current->mm, logical);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return GCERR_MMU_PAGE_BAD;

	/* Get the pointer to the entry in PMD for the address. */
	pmd = pmd_offset(pgd, logical);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return GCERR_MMU_PAGE_BAD;

	/* Get the pointer to the entry in PTE for the address. */
	pte = pte_offset_map(pmd, logical);
	if ((pte == NULL) || !pte_present(*pte))
		return GCERR_MMU_PAGE_BAD;

	*physical = (*pte & PAGE_MASK) | (logical & ~PAGE_MASK);
	return GCERR_NONE;
}

static enum gcerror get_physical_pages(struct mmu2dphysmem *mem,
					pte_t *parray,
					struct mmu2darena *arena)
{
	enum gcerror gcerror = GCERR_NONE;
	struct vm_area_struct *vma;
	struct page **pages = NULL;
	u32 base, write;
	int i, count = 0;

	/* Reset page descriptor array. */
	arena->pages = NULL;

	/* Get base address shortcut. */
	base = mem->base;

	/* Store the logical pointer. */
	arena->logical = (void *) base;

	/*
	 * Important Note: base is mapped from user application process
	 * to current process - it must lie completely within the current
	 * virtual memory address space in order to be of use to us here.
	 */
	vma = find_vma(current->mm, base + (mem->count << PAGE_SHIFT) - 1);
	if ((vma == NULL) || (base < vma->vm_start)) {
		gcerror = GCERR_MMU_BUFFER_BAD;
		goto exit;
	}

	/* Allocate page descriptor array. */
	pages = kmalloc(mem->count * sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL) {
		gcerror = GCERR_SETGRP(GCERR_OODM, GCERR_MMU_DESC_ALLOC);
		goto exit;
	}

	/* Query page descriptors. */
	write = ((vma->vm_flags & (VM_WRITE | VM_MAYWRITE)) != 0) ? 1 : 0;
	count = get_user_pages(current, current->mm, base, mem->count,
				write, 1, pages, NULL);

	if (count < 0) {
		/* Kernel allocated buffer. */
		for (i = 0; i < mem->count; i += 1) {
			gcerror = virt2phys(base, &parray[i]);
			if (gcerror != GCERR_NONE)
				goto exit;

			base += mem->pagesize;
		}
	} else if (count == mem->count) {
		/* User allocated buffer. */
		for (i = 0; i < mem->count; i += 1) {
			parray[i] = page_to_phys(pages[i]);
			if (phys_to_page(parray[i]) != pages[i]) {
				gcerror = GCERR_MMU_PAGE_BAD;
				goto exit;
			}
		}

		/* Set page descriptor array. */
		arena->pages = pages;
	} else {
		gcerror = GCERR_MMU_BUFFER_BAD;
		goto exit;
	}

exit:
	if (arena->pages == NULL) {
		for (i = 0; i < count; i += 1)
			page_cache_release(pages[i]);

		if (pages != NULL)
			kfree(pages);
	}

	return gcerror;
}

static void release_physical_pages(struct mmu2darena *arena)
{
	u32 i;

	if (arena->pages != NULL) {
		for (i = 0; i < arena->count; i += 1)
			page_cache_release(arena->pages[i]);

		kfree(arena->pages);
		arena->pages = NULL;
	}
}

static void flush_user_buffer(struct mmu2darena *arena)
{
	u32 i;
	struct gcpage gcpage;
	unsigned char *logical;

	if (arena->pages == NULL) {
		GC_PRINT(KERN_ERR "%s(%d): page array is NULL.\n",
			__func__, __LINE__);
		return;
	}


	logical = arena->logical;
	if (logical == NULL) {
		GC_PRINT(KERN_ERR "%s(%d): buffer base is NULL.\n",
			__func__, __LINE__);
			return;
	}

	for (i = 0; i < arena->count; i += 1) {
		gcpage.order = get_order(PAGE_SIZE);
		gcpage.size = PAGE_SIZE;

		gcpage.pages = arena->pages[i];
		if (gcpage.pages == NULL) {
			GC_PRINT(KERN_ERR
				"%s(%d): page structure %d is NULL.\n",
				__func__, __LINE__, i);
			continue;
		}

		gcpage.physical = page_to_phys(gcpage.pages);
		if (gcpage.physical == 0) {
			GC_PRINT(KERN_ERR
				"%s(%d): physical address of page %d is 0.\n",
				__func__, __LINE__, i);
			continue;
		}

		gcpage.logical = (unsigned int *) (logical + i * PAGE_SIZE);
		if (gcpage.logical == NULL) {
			GC_PRINT(KERN_ERR
				"%s(%d): virtual address of page %d is NULL.\n",
				__func__, __LINE__, i);
			continue;
		}

		gc_flush_pages(&gcpage);
	}
}

enum gcerror mmu2d_map(struct mmu2dcontext *ctxt, struct mmu2dphysmem *mem,
			struct mmu2darena **mapped)
{
	enum gcerror gcerror = GCERR_NONE;
	struct mmu2darena *prev, *vacant, *split;
#if MMU_ENABLE
	struct mmu2dstlb *stlb = NULL;
	u32 *mtlb_logical, *stlb_logical;
#endif
	u32 mtlb_idx, stlb_idx, next_idx;
#if MMU_ENABLE
	u32 i, j, count, available;
#else
	u32 i, count, available;
#endif
	pte_t *parray_alloc = NULL;
	pte_t *parray;

	if ((ctxt == NULL) || (ctxt->mmu == NULL))
		return GCERR_MMU_CTXT_BAD;

	if ((mem == NULL) || (mem->count <= 0) || (mapped == NULL) ||
		((mem->pagesize != 0) && (mem->pagesize != MMU_PAGE_SIZE)))
		return GCERR_MMU_ARG;

	down_read(&current->mm->mmap_sem);

	/*
	 * Find available sufficient arena.
	 */

	prev = NULL;
	vacant = ctxt->vacant;

	while (vacant != NULL) {
		if (vacant->count >= mem->count)
			break;
		prev = vacant;
		vacant = vacant->next;
	}

	if (vacant == NULL) {
		gcerror = GCERR_MMU_OOM;
		goto fail;
	}

	/*
	 * Create page array.
	 */

	/* Reset page array. */
	vacant->pages = NULL;

	/* No page array given? */
	if (mem->pages == NULL) {
		/* Allocate physical address array. */
		parray_alloc = kmalloc(mem->count * sizeof(pte_t *),
					GFP_KERNEL);
		if (parray_alloc == NULL) {
			gcerror = GCERR_SETGRP(GCERR_OODM,
						GCERR_MMU_PHYS_ALLOC);
			goto fail;
		}

		/* Fetch page addresses. */
		gcerror = get_physical_pages(mem, parray_alloc, vacant);
		if (gcerror != GCERR_NONE)
			goto fail;

		parray = parray_alloc;
	} else
		parray = mem->pages;

#if GC_DUMP
	GC_PRINT(KERN_ERR "%s(%d): mapping (%d) pages:\n",
		__func__, __LINE__, mem->count);
#endif

	/*
	 * Allocate slave tables as necessary.
	 */

	mtlb_idx = vacant->mtlb;
	stlb_idx = vacant->stlb;
	count = mem->count;

#if MMU_ENABLE
	mtlb_logical = &ctxt->master.logical[mtlb_idx];
#endif

	for (i = 0; count > 0; i += 1) {
#if MMU_ENABLE
		if (mtlb_logical[i] == MMU_MTLB_ENTRY_VACANT) {
			gcerror = mmu2d_allocate_slave(ctxt, &stlb);
			if (gcerror != GCERR_NONE)
				goto fail;

			mtlb_logical[i]
				= (stlb->pages.physical & MMU_MTLB_SLAVE_MASK)
				| MMU_MTLB_4K_PAGE
				| MMU_MTLB_EXCEPTION
				| MMU_MTLB_PRESENT;

			ctxt->slave[i] = stlb;
		}
#endif

		available = MMU_STLB_ENTRY_NUM - stlb_idx;

		if (available > count) {
			available = count;
			next_idx = stlb_idx + count;
		} else {
			mtlb_idx += 1;
			next_idx = 0;
		}

#if MMU_ENABLE
		stlb_logical = &ctxt->slave[i]->pages.logical[stlb_idx];
		ctxt->slave[i]->count += available;

		for (j = 0; j < available; j += 1) {
			stlb_logical[j]
				= (*parray & MMU_STLB_ADDRESS_MASK)
				| MMU_STLB_PRESENT
				| MMU_STLB_EXCEPTION
				| MMU_STLB_WRITEABLE;

			parray += 1;
		}

		gc_flush_pages(&ctxt->slave[i]->pages);
#endif

		count -= available;
		stlb_idx = next_idx;
	}

#if MMU_ENABLE
	gc_flush_pages(&ctxt->master);
#endif

	/*
	 * Claim arena.
	 */

	mem->pagesize = MMU_PAGE_SIZE;

	if (vacant->count != mem->count) {
		gcerror = mmu2d_get_arena(ctxt->mmu, &split);
		if (gcerror != GCERR_NONE)
			goto fail;

		split->mtlb  = mtlb_idx;
		split->stlb  = stlb_idx;
		split->count = vacant->count - mem->count;
		split->next  = vacant->next;
		vacant->next = split;
		vacant->count = mem->count;
	}

	if (prev == NULL)
		ctxt->vacant = vacant->next;
	else
		prev->next = vacant->next;

	vacant->next = ctxt->allocated;
	ctxt->allocated = vacant;

	*mapped = vacant;

#if MMU_ENABLE
	vacant->address
		= ((vacant->mtlb << MMU_MTLB_SHIFT) & MMU_MTLB_MASK)
		| ((vacant->stlb << MMU_STLB_SHIFT) & MMU_STLB_MASK)
		| (mem->offset & MMU_OFFSET_MASK);
#else
	vacant->address = mem->offset + ((parray_alloc == NULL)
		? *mem->pages : *parray_alloc);
#endif

fail:
	if (parray_alloc != NULL) {
		kfree(parray_alloc);

		if (gcerror != GCERR_NONE)
			release_physical_pages(vacant);
	}

	up_read(&current->mm->mmap_sem);
	return gcerror;
}

enum gcerror mmu2d_unmap(struct mmu2dcontext *ctxt, struct mmu2darena *mapped)
{
	enum gcerror gcerror = GCERR_NONE;
	struct mmu2darena *prev, *allocated, *vacant;
#if MMU_ENABLE
	struct mmu2dstlb *stlb;
#endif
	u32 mtlb_idx, stlb_idx;
	u32 next_mtlb_idx, next_stlb_idx;
#if MMU_ENABLE
	u32 i, j, count, available;
	u32 *stlb_logical;
#else
	u32 i, count, available;
#endif

	if ((ctxt == NULL) || (ctxt->mmu == NULL))
		return GCERR_MMU_CTXT_BAD;

	down_read(&current->mm->mmap_sem);

	/*
	 * Find the arena.
	 */

	prev = NULL;
	allocated = ctxt->allocated;

	while (allocated != NULL) {
		if (allocated == mapped)
			break;
		prev = allocated;
		allocated = allocated->next;
	}

	/* The allocation is not listed. */
	if (allocated == NULL) {
		gcerror = GCERR_MMU_ARG;
		goto fail;
	}

	mtlb_idx = allocated->mtlb;
	stlb_idx = allocated->stlb;

	/*
	 * Free slave tables.
	 */

	count = allocated->count;

	for (i = 0; count > 0; i += 1) {
		available = MMU_STLB_ENTRY_NUM - stlb_idx;

		if (available > count) {
			available = count;
			next_mtlb_idx = mtlb_idx;
			next_stlb_idx = stlb_idx + count;
		} else {
			next_mtlb_idx = mtlb_idx + 1;
			next_stlb_idx = 0;
		}

#if MMU_ENABLE
		stlb = ctxt->slave[mtlb_idx];
		if (stlb == NULL) {
			gcerror = GCERR_MMU_ARG;
			goto fail;
		}

		if (stlb->count < available) {
			gcerror = GCERR_MMU_ARG;
			goto fail;
		}

		stlb_logical = &stlb->pages.logical[stlb_idx];
		for (j = 0; j < available; j += 1)
			stlb_logical[j] = MMU_STLB_ENTRY_VACANT;

		stlb->count -= available;
		if (stlb->count == 0) {
			mmu2d_free_slave(ctxt, stlb);
			ctxt->slave[mtlb_idx] = NULL;
			ctxt->master.logical[mtlb_idx] = MMU_MTLB_ENTRY_VACANT;
		}
#endif

		count -= available;
		mtlb_idx = next_mtlb_idx;
		stlb_idx = next_stlb_idx;
	}

	/*
	 * Remove from allocated arenas.
	 */

	if (prev == NULL)
		ctxt->allocated = allocated->next;
	else
		prev->next = allocated->next;

	release_physical_pages(allocated);

	/*
	 * Find point of insertion for the arena.
	 */

	prev = NULL;
	vacant = ctxt->vacant;

	while (vacant != NULL) {
		if ((vacant->mtlb >= allocated->mtlb) &&
				(vacant->stlb > allocated->stlb))
			break;
		prev = vacant;
		vacant = vacant->next;
	}

	if (prev == NULL) {
		if (vacant == NULL) {
			allocated->next = ctxt->vacant;
			ctxt->vacant = allocated;
		} else {
			if (mmu2d_siblings(allocated, vacant)) {
				vacant->mtlb   = allocated->mtlb;
				vacant->stlb   = allocated->stlb;
				vacant->count += allocated->count;
				mmu2d_free_arena(ctxt->mmu, allocated);
			} else {
				allocated->next = ctxt->vacant;
				ctxt->vacant = allocated;
			}
		}
	} else {
		if (mmu2d_siblings(prev, allocated)) {
			if (mmu2d_siblings(allocated, vacant)) {
				prev->count += allocated->count;
				prev->count += vacant->count;
				prev->next   = vacant->next;
				mmu2d_free_arena(ctxt->mmu, allocated);
				mmu2d_free_arena(ctxt->mmu, vacant);
			} else {
				prev->count += allocated->count;
				mmu2d_free_arena(ctxt->mmu, allocated);
			}
		} else if (mmu2d_siblings(allocated, vacant)) {
			vacant->mtlb   = allocated->mtlb;
			vacant->stlb   = allocated->stlb;
			vacant->count += allocated->count;
			mmu2d_free_arena(ctxt->mmu, allocated);
		} else {
			allocated->next = vacant;
			prev->next = allocated;
		}
	}

fail:
	up_read(&current->mm->mmap_sem);
	return gcerror;
}

int mmu2d_flush(u32 *logical, u32 address, u32 size)
{
#if MMU_ENABLE
	static const int flushSize = 16 * sizeof(u32);
	u32 count;

	if (logical != NULL) {
		/* Compute the buffer count. */
		count = (size - flushSize + 7) >> 3;

		/* Flush 2D PE cache. */
		logical[0] = LS(AQFlushRegAddrs, 1);
		logical[1] = SETFIELDVAL(0, AQ_FLUSH, PE2D_CACHE, ENABLE);

		/* Arm the FE-PE semaphore. */
		logical[2] = LS(AQSemaphoreRegAddrs, 1);

		logical[3]
		= SETFIELDVAL(0, AQ_SEMAPHORE, SOURCE, FRONT_END)
		| SETFIELDVAL(0, AQ_SEMAPHORE, DESTINATION, PIXEL_ENGINE);

		/* Stall FE until PE is done flushing. */
		logical[4]
		= SETFIELDVAL(0, STALL_COMMAND, OPCODE, STALL);

		logical[5]
		= SETFIELDVAL(0, AQ_SEMAPHORE, SOURCE, FRONT_END)
		| SETFIELDVAL(0, AQ_SEMAPHORE, DESTINATION, PIXEL_ENGINE);

		/* LINK to the next slot to flush FE FIFO. */
		logical[6]
		= SETFIELDVAL(0, AQ_COMMAND_LINK_COMMAND, OPCODE, LINK)
		| SETFIELD(0, AQ_COMMAND_LINK_COMMAND, PREFETCH, 4);

		logical[7] = address + 8 * sizeof(u32);

		/* Flush MMU cache. */
		logical[8] = LS(gcregMMUConfigurationRegAddrs, 1);

		logical[9]
		= SETFIELDVAL(~0U, GCREG_MMU_CONFIGURATION, FLUSH, FLUSH)
		& SETFIELDVAL(~0U, GCREG_MMU_CONFIGURATION, MASK_FLUSH, ENABLED)
		;

		/* Arm the FE-PE semaphore. */
		logical[10] = LS(AQSemaphoreRegAddrs, 1);

		logical[11]
		= SETFIELDVAL(0, AQ_SEMAPHORE, SOURCE, FRONT_END)
		| SETFIELDVAL(0, AQ_SEMAPHORE, DESTINATION, PIXEL_ENGINE);

		/* Stall FE until PE is done flushing. */
		logical[12]
		= SETFIELDVAL(0, STALL_COMMAND, OPCODE, STALL);

		logical[13]
		= SETFIELDVAL(0, AQ_SEMAPHORE, SOURCE, FRONT_END)
		| SETFIELDVAL(0, AQ_SEMAPHORE, DESTINATION, PIXEL_ENGINE);

		/* LINK to the next slot to flush FE FIFO. */
		logical[14]
		= SETFIELDVAL(0, AQ_COMMAND_LINK_COMMAND, OPCODE, LINK)
		| SETFIELD(0, AQ_COMMAND_LINK_COMMAND, PREFETCH, count);

		logical[15]
		= address + flushSize;
	}

	/* Return the size in bytes required for the flush. */
	return flushSize;
#else
	return 0;
#endif
}

enum gcerror mmu2d_fixup(
	struct gcfixup *fixup, unsigned int *data
	)
{
	enum gcerror gcerror = GCERR_NONE;
	static struct gcfixup _fixup;	/* FIXME/TODO */
	int fixedsize, tablesize;
	struct mmu2darena *arena;
	unsigned int *table;
	unsigned int offset;
	unsigned int i;

	/* Get the fixed sized of the structure. */
	fixedsize = offsetof(struct gcfixup, fixup);

	/* Process fixups. */
	while (fixup != NULL) {

		/* Get gcfixup structure up to the table. */
		if (copy_from_user(&_fixup, fixup, fixedsize)) {
			gcerror = GCERR_USER_READ;
			goto exit;
		}

		/* Compute the size of the fixup table. */
		tablesize = _fixup.count * sizeof(unsigned int);

		/* Get the fixup table. */
		if (copy_from_user(&_fixup.fixup, fixup->fixup, tablesize)) {
			gcerror = GCERR_USER_READ;
			goto exit;
		}

		fixup = &_fixup;
		table = _fixup.fixup;

		/* Apply fixups. */
		for (i = 0; i < fixup->count; i += 1) {
			offset = *table++;
			arena = (struct mmu2darena *) data[offset];
			data[offset] = arena->address;
			flush_user_buffer(arena);
		}

		/* Get the next fixup. */
		fixup = fixup->next;
	}

exit:
	return gcerror;
}

void mmu2d_dump(struct mmu2dcontext *ctxt)
{
	static struct mm2dtable mtlb_desc = {
		"Master",
		MMU_MTLB_ENTRY_NUM,
		MMU_MTLB_ENTRY_VACANT,
		get_mtlb_present,
		print_mtlb_entry
	};

	static struct mm2dtable stlb_desc = {
		"Slave",
		MMU_STLB_ENTRY_NUM,
		MMU_STLB_ENTRY_VACANT,
		get_stlb_present,
		print_stlb_entry
	};

	struct mmu2darena *vacant;
	u32 size;
	char *unit;
	int i;

	GC_PRINT(KERN_ERR "\n*** MMU DUMP ***\n");

	if (ctxt->vacant == NULL) {
		GC_PRINT(KERN_ERR "\nNo vacant arenas defined!\n");
	} else {
		vacant = ctxt->vacant;

		while (vacant != NULL) {

			size = vacant->count * 4;

			if (size < 1024) {
				unit = "KB";
			} else {
				size /= 1024;
				if (size < 1024) {
					unit = "MB";
				} else {
					size /= 1024;
					unit = "GB";
				}
			}

			GC_PRINT(KERN_ERR
				"Vacant arena: 0x%08X\n", (u32) vacant);

			GC_PRINT(KERN_ERR
				"  mtlb       = %d\n", vacant->mtlb);

			GC_PRINT(KERN_ERR
				"  stlb       = %d\n", vacant->stlb);

			GC_PRINT(KERN_ERR
				"  page count = %d\n", vacant->count);

			GC_PRINT(KERN_ERR
				"  size       = %d%s\n", size, unit);

			vacant = vacant->next;
		}
	}

	mmu2d_dump_table(&mtlb_desc, &ctxt->master);

	for (i = 0; i < MMU_MTLB_ENTRY_NUM; i += 1)
		if (ctxt->slave[i] != NULL)
			mmu2d_dump_table(&stlb_desc, &ctxt->slave[i]->pages);
}
