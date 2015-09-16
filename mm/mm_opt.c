/*
 *  linux/mm/mm_opt.c
 *
 *  Copyright (C) 2015  Yizheng Jiao
 */

#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/pfn.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/highmem.h>

#include "internal.h"

#define MM_OPT_REGION_ORDER	4U

static bool free_pages_prepare_mm_opt(struct page *page)
{
	if (page_mapcount(page) != 0 || page->mapping != NULL
		|| page_count(page) != 0) {
		dump_page(page);
		BUG();
	}

	set_page_count(page, 1);
	page->flags = 0x40000000;
	
	return true;
}

static void prep_compound_page_mm_opt(struct page *page,
		unsigned long order)
{
	int i;
	int nr_pages = 1 << order;

	set_page_count(page, 1);
	page->reg = NULL;
	page->flags = 0x40000000;
	page->index = 0x0;
	page->mapping = NULL;

	for (i = 1; i < nr_pages; i++) {
		struct page *p = &page[i];

		p->flags = 0x40000000;
		p->index = 0x0;
		p->mapping = NULL;
		set_page_count(p, 0);
		p->reg = NULL;
	}
}

/* This is for sanity check, when eveything 
 * works fine disable it 
 */
static void check_free_region(struct mm_region *reg)
{
	struct page *page;
	unsigned long pfn1 = page_to_pfn(reg->head);	
	unsigned long pfn2;
	int flags[1 << MM_OPT_REGION_ORDER];
	int size = 1 << MM_OPT_REGION_ORDER;
	int i = 0;
	int j = 0;

	for (j = 0; j < size; j++)
		flags[j] = 0;

	list_for_each_entry(page, &reg->freelist, lru) {
		pfn2 = page_to_pfn(page);
		BUG_ON(pfn2 < pfn1 || pfn2 > pfn1 + size - 1);
		BUG_ON(flags[pfn2 - pfn1] == 1);
		flags[pfn2 - pfn1] = 1;
		i++;
	}
	BUG_ON(i != size);
}

static void mm_region_free(struct mm_region *reg)
{
	unsigned int order = MM_OPT_REGION_ORDER;

	check_free_region(reg);
	__free_pages(reg->head, order);
}

static int destroy_compound_page_mm_opt(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;
	
	if (page_count(page) != 1) {
		dump_page(page);
		BUG();
	}
	set_page_count(page, 1);
	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;
		if (page_count(p) != 0) {
			dump_page(p);
			BUG();
		}
		set_page_count(p, 1);
	}

	return 0;
}

/* Alloc a new region from buddy system */
static struct mm_region *mm_alloc_region(gfp_t gfp_mask, struct mm_domain *dom)
{
	int bad;
	struct page *page; 
	struct mm_region *reg = (struct mm_region *)
		kmalloc(sizeof(struct mm_region), GFP_KERNEL);

	if (reg == NULL)
		return NULL;
	page = alloc_pages(gfp_mask, MM_OPT_REGION_ORDER);
	if (page == NULL) {
		kfree(reg);
		return NULL;
	}
	
	bad = destroy_compound_page_mm_opt(page, MM_OPT_REGION_ORDER);
	reg->head = page;
	reg->size = 1 << MM_OPT_REGION_ORDER;
	reg->index = 0;
	INIT_LIST_HEAD(&reg->freelist);
	INIT_LIST_HEAD(&reg->domlist);
	reg->freesize = 0;
	reg->dom = dom;	

	return reg;
}

/* Try to allocate a page from NON-empty region */
static struct page* mm_region_alloc_page(struct mm_region *reg, gfp_t gfp_mask)
{
	struct list_head *freelist = &reg->freelist;
	struct page *page = NULL;
	

	if (reg->freesize > 0) {
		page = list_entry(freelist->next, struct page, lru);
		list_del(&page->lru);
		page->reg = reg;
		reg->freesize--;
	} else if (reg->index < reg->size) {
		page = reg->head + reg->index;
		page->reg = reg;
		reg->index++;
	}

	if ((gfp_mask & __GFP_ZERO) && page)
		clear_highpage(page);
	return page;
}

int mm_region_free_page(struct page *page)
{
	struct mm_region *reg = page->reg;

	free_pages_prepare_mm_opt(page);
	list_add(&page->lru, &reg->freelist);
	reg->freesize++;

	if (reg->freesize == reg->size) {
		prep_compound_page_mm_opt(reg->head, MM_OPT_REGION_ORDER);
		mm_region_free(reg);
		list_del(&reg->domlist);
		if (reg->dom->cache_reg == reg)
			reg->dom->cache_reg = NULL;
		reg->dom->size--;
		reg->head = NULL;
		kfree(reg);
	}
	return 0;
}

/* Check whether a region is full */
static bool mm_region_is_full(struct mm_region *reg)
{
	VM_BUG_ON(reg == NULL);
	if (reg->index == reg->size && reg->freesize == 0)
		return true;
	return false;
}

/*
 * Check whether a domain is full. Domain is full 
 * when all regions are full
 */
static bool mm_domain_is_full(struct mm_domain *dom)
{
	struct mm_region *reg;

	VM_BUG_ON(dom == NULL);
	list_for_each_entry(reg, &dom->domlist_head, domlist) {
		if (!mm_region_is_full(reg))
			return false;
	}
	
	return true;
}

/* Return a region that is not full */
static struct mm_region *mm_domain_find_region(struct mm_domain *dom)
{
	struct mm_region *reg;

	VM_BUG_ON(dom == NULL);
	list_for_each_entry(reg, &dom->domlist_head, domlist) {
		if (!mm_region_is_full(reg))
			return reg;
	}
	
	return NULL;
}

/* Hijake virtual process page allocation */
struct page *alloc_pages_vma_mm_opt(gfp_t gfp_mask, int order,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm;
	struct mm_domain *dom;
	struct page *page;
	
	VM_BUG_ON(order != 0);
	
	mm = vma->vm_mm;
	dom = mm->vmdomain;
	
	if (dom->size == 0 || mm_domain_is_full(dom)) {
		struct mm_region *reg = mm_alloc_region(gfp_mask, dom);

		if (reg == NULL)
			goto normal;
		list_add(&reg->domlist, &dom->domlist_head);
		dom->size++;
		dom->cache_reg = reg;
	}

	if (dom->cache_reg == NULL || mm_region_is_full(dom->cache_reg)) {
		struct mm_region *reg = mm_domain_find_region(dom);

		if (reg == NULL)
			goto normal;
		dom->cache_reg = reg;
	}

	page = mm_region_alloc_page(dom->cache_reg, gfp_mask);
	if (page == NULL) 
		goto normal;
	return page;
normal:
	page = alloc_pages(gfp_mask, order);
	return page;
}

/* Hijack page cache allocation */
struct page *__page_cache_alloc_mm_opt(gfp_t gfp,
			struct address_space *x)
{
	return alloc_pages(gfp, 0);
}
