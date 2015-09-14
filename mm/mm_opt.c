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

#include "internal.h"

#define MM_OPT_REGION_ORDER	4U

static bool free_pages_prepare_mm_opt(struct page *page)
{
	if (page_mapcount(page) != 0) {
		dump_page(page);
		BUG();
	}

	set_page_count(page, 1);

	page->flags = 0x40000000;
	page->mapping = NULL;
	page->index = 0x0;
	page->private = 0;

	return true;
}

static void prep_compound_page_mm_opt(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;

	set_page_count(page, 0);
	for (i = 1; i < nr_pages; i++) {
		struct page *p = &page[i];
		
		set_page_count(p, 0);
		p->reg = NULL;
	}
}

static void mm_region_free(struct mm_region *reg)
{
#if 0
	int i;
	int nr_pages = 1 << MM_OPT_REGION_ORDER;

	for (i = 0; i < nr_pages; i++) {
		struct page *p = &reg->head[i];

		__free_pages(p, 0);
	}
#endif
	unsigned int order = MM_OPT_REGION_ORDER;
	__free_pages(reg->head, order);
}

static int destroy_compound_page_mm_opt(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;
	
	set_page_count(page, 1);
	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;

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

/* Check whether a region is full */
static bool mm_region_is_full(struct mm_region *reg)
{
	BUG_ON(reg == NULL);
	if (reg->index == reg->size && reg->freesize == 0)
		return true;
	return false;
}

/* Try to allocate a page from NON-empty region */
static struct page* mm_region_alloc_page(struct mm_region *reg)
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
		kfree(reg);
	}
	return 0;
}

/*
 * Check whether a domain is full. Domain is full 
 * when all regions are full
 */
static bool mm_domain_is_full(struct mm_domain *dom)
{
	struct mm_region *reg;

	BUG_ON(dom == NULL);
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

	BUG_ON(dom == NULL);
	list_for_each_entry(reg, &dom->domlist_head, domlist) {
		if (!mm_region_is_full(reg))
			return reg;
	}
	
	return NULL;
}

struct page *alloc_pages_vma_mm_opt(gfp_t gfp_mask, int order,
		struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm;
	struct mm_domain *dom;
	struct page *page;
	
	goto normal;
	BUG_ON(order != 0);

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

	page = mm_region_alloc_page(dom->cache_reg);
	if (page == NULL) 
		goto normal;
	return page;
normal:
	page = alloc_pages(gfp_mask, order);
	return page;
}
