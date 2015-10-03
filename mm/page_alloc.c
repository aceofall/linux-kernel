﻿/*
 *  linux/mm/page_alloc.c
 *
 *  Manages the free list, the system allocates free pages here.
 *  Note that kmalloc() lives in slab.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  Reshaped it to be a zoned allocator, Ingo Molnar, Red Hat, 1999
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *  Zone balancing, Kanoj Sarcar, SGI, Jan 2000
 *  Per cpu hot/cold page lists, bulk allocation, Martin J. Bligh, Sept 2002
 *          (lots of bits borrowed from Ingo Molnar & Andrew Morton)
 */

#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/jiffies.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/kmemcheck.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/oom.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memory_hotplug.h>
#include <linux/nodemask.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mempolicy.h>
#include <linux/stop_machine.h>
#include <linux/sort.h>
#include <linux/pfn.h>
#include <linux/backing-dev.h>
#include <linux/fault-inject.h>
#include <linux/page-isolation.h>
#include <linux/page_cgroup.h>
#include <linux/debugobjects.h>
#include <linux/kmemleak.h>
#include <linux/compaction.h>
#include <trace/events/kmem.h>
#include <linux/ftrace_event.h>
#include <linux/memcontrol.h>
#include <linux/prefetch.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/page-debug-flags.h>
#include <linux/hugetlb.h>
#include <linux/sched/rt.h>

#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/div64.h>
#include "internal.h"

/* prevent >1 _updater_ of zone percpu pageset ->high and ->batch fields */
static DEFINE_MUTEX(pcp_batch_high_lock);

#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
DEFINE_PER_CPU(int, numa_node);
EXPORT_PER_CPU_SYMBOL(numa_node);
#endif

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * N.B., Do NOT reference the '_numa_mem_' per cpu variable directly.
 * It will not be defined when CONFIG_HAVE_MEMORYLESS_NODES is not defined.
 * Use the accessor functions set_numa_mem(), numa_mem_id() and cpu_to_mem()
 * defined in <linux/topology.h>.
 */
DEFINE_PER_CPU(int, _numa_mem_);		/* Kernel "local memory" node */
EXPORT_PER_CPU_SYMBOL(_numa_mem_);
#endif

/*
 * Array of node states.
 */
// ARM10C 20140426
nodemask_t node_states[NR_NODE_STATES] __read_mostly = {
	[N_POSSIBLE] = NODE_MASK_ALL,
	[N_ONLINE] = { { [0] = 1UL } },
#ifndef CONFIG_NUMA // CONFIG_NUMA=n
	[N_NORMAL_MEMORY] = { { [0] = 1UL } },
#ifdef CONFIG_HIGHMEM // CONFIG_HIGHMEM=y
	[N_HIGH_MEMORY] = { { [0] = 1UL } },
#endif
#ifdef CONFIG_MOVABLE_NODE // CONFIG_MOVABLE_NODE=n
	[N_MEMORY] = { { [0] = 1UL } },
#endif
	[N_CPU] = { { [0] = 1UL } },
#endif	/* NUMA */
};
EXPORT_SYMBOL(node_states);

/* Protect totalram_pages and zone->managed_pages */
static DEFINE_SPINLOCK(managed_page_count_lock);

// ARM10C 20140412
// ARM10C 20140419
// ARM10C 20150919
unsigned long totalram_pages __read_mostly;
unsigned long totalreserve_pages __read_mostly;
/*
 * When calculating the number of globally allowed dirty pages, there
 * is a certain number of per-zone reserves that should not be
 * considered dirtyable memory.  This is the sum of those reserves
 * over all existing zones that contribute dirtyable memory.
 */
unsigned long dirty_balance_reserve __read_mostly;

// ARM10C 20150912
int percpu_pagelist_fraction;
// ARM10C 20140426
// ARM10C 20140614
// ARM10C 20140621
// GFP_BOOT_MASK: 0x1ffff2f
// gfp_allowed_mask: 0x1ffff2f
gfp_t gfp_allowed_mask __read_mostly = GFP_BOOT_MASK;

#ifdef CONFIG_PM_SLEEP
/*
 * The following functions are used by the suspend/hibernate code to temporarily
 * change gfp_allowed_mask in order to avoid using I/O during memory allocations
 * while devices are suspended.  To avoid races with the suspend/hibernate code,
 * they should always be called with pm_mutex held (gfp_allowed_mask also should
 * only be modified with pm_mutex held, unless the suspend/hibernate code is
 * guaranteed not to run in parallel with that modification).
 */

static gfp_t saved_gfp_mask;

void pm_restore_gfp_mask(void)
{
	WARN_ON(!mutex_is_locked(&pm_mutex));
	if (saved_gfp_mask) {
		gfp_allowed_mask = saved_gfp_mask;
		saved_gfp_mask = 0;
	}
}

void pm_restrict_gfp_mask(void)
{
	WARN_ON(!mutex_is_locked(&pm_mutex));
	WARN_ON(saved_gfp_mask);
	saved_gfp_mask = gfp_allowed_mask;
	gfp_allowed_mask &= ~GFP_IOFS;
}

bool pm_suspended_storage(void)
{
	if ((gfp_allowed_mask & GFP_IOFS) == GFP_IOFS)
		return false;
	return true;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE
int pageblock_order __read_mostly;
#endif

static void __free_pages_ok(struct page *page, unsigned int order);

/*
 * results with 256, 32 in the lowmem_reserve sysctl:
 *	1G machine -> (16M dma, 800M-16M normal, 1G-800M high)
 *	1G machine -> (16M dma, 784M normal, 224M high)
 *	NORMAL allocation will leave 784M/256 of ram reserved in the ZONE_DMA
 *	HIGHMEM allocation will leave 224M/32 of ram reserved in ZONE_NORMAL
 *	HIGHMEM allocation will (224M+784M)/256 of ram reserved in ZONE_DMA
 *
 * TBD: should special case ZONE_DMA32 machines here - in those we normally
 * don't need any ZONE_NORMAL reservation
 */
int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES-1] = {
#ifdef CONFIG_ZONE_DMA
	 256,
#endif
#ifdef CONFIG_ZONE_DMA32
	 256,
#endif
#ifdef CONFIG_HIGHMEM
	 32,
#endif
	 32,
};

EXPORT_SYMBOL(totalram_pages);

// ARM10C 20140111 
static char * const zone_names[MAX_NR_ZONES] = {
#ifdef CONFIG_ZONE_DMA
	 "DMA",
#endif
#ifdef CONFIG_ZONE_DMA32
	 "DMA32",
#endif
	 "Normal",
#ifdef CONFIG_HIGHMEM
	 "HighMem",
#endif
	 "Movable",
};

int min_free_kbytes = 1024;
int user_min_free_kbytes;

static unsigned long __meminitdata nr_kernel_pages;
static unsigned long __meminitdata nr_all_pages;
static unsigned long __meminitdata dma_reserve;

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
static unsigned long __meminitdata arch_zone_lowest_possible_pfn[MAX_NR_ZONES];
static unsigned long __meminitdata arch_zone_highest_possible_pfn[MAX_NR_ZONES];
static unsigned long __initdata required_kernelcore;
static unsigned long __initdata required_movablecore;
static unsigned long __meminitdata zone_movable_pfn[MAX_NUMNODES];

/* movable_zone is the "real" zone pages in ZONE_MOVABLE are taken from */
int movable_zone;
EXPORT_SYMBOL(movable_zone);
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

#if MAX_NUMNODES > 1
int nr_node_ids __read_mostly = MAX_NUMNODES;
int nr_online_nodes __read_mostly = 1;
EXPORT_SYMBOL(nr_node_ids);
EXPORT_SYMBOL(nr_online_nodes);
#endif

// ARM10C 20140426
// ARM10C 20140517
int page_group_by_mobility_disabled __read_mostly;

// ARM10C 20140118
// MIGRATE_MOVABLE : 2
// ARM10C 20140517
// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page, start_type: 0
void set_pageblock_migratetype(struct page *page, int migratetype)
{
	// 수행 안함
	// page_group_by_mobility_disabled: 0, MIGRATE_PCPTYPES: 3
	if (unlikely(page_group_by_mobility_disabled &&
		     migratetype < MIGRATE_PCPTYPES))
		migratetype = MIGRATE_UNMOVABLE;
	
	// migratetype : 2, PB_migrate : 0, PB_migrate_end : 2
	// migratetype : 0, PB_migrate : 0, PB_migrate_end : 2
	set_pageblock_flags_group(page, (unsigned long)migratetype,
					PB_migrate, PB_migrate_end);
}

bool oom_killer_disabled __read_mostly;

#ifdef CONFIG_DEBUG_VM // CONFIG_DEBUG_VM=n
static int page_outside_zone_boundaries(struct zone *zone, struct page *page)
{
	int ret = 0;
	unsigned seq;
	unsigned long pfn = page_to_pfn(page);
	unsigned long sp, start_pfn;

	do {
		seq = zone_span_seqbegin(zone);
		start_pfn = zone->zone_start_pfn;
		sp = zone->spanned_pages;
		if (!zone_spans_pfn(zone, pfn))
			ret = 1;
	} while (zone_span_seqretry(zone, seq));

	if (ret)
		pr_err("page %lu outside zone [ %lu - %lu ]\n",
			pfn, start_pfn, start_pfn + sp);

	return ret;
}

static int page_is_consistent(struct zone *zone, struct page *page)
{
	if (!pfn_valid_within(page_to_pfn(page)))
		return 0;
	if (zone != page_zone(page))
		return 0;

	return 1;
}
/*
 * Temporary debugging check for pages not lying within a given zone.
 */
static int bad_range(struct zone *zone, struct page *page)
{
	if (page_outside_zone_boundaries(zone, page))
		return 1;
	if (!page_is_consistent(zone, page))
		return 1;

	return 0;
}
#else
// ARM10C 20140405
// ARM10C 20140517
// ARM10C 20140524
static inline int bad_range(struct zone *zone, struct page *page)
{
	return 0;
}
#endif

static void bad_page(struct page *page)
{
	static unsigned long resume;
	static unsigned long nr_shown;
	static unsigned long nr_unshown;

	/* Don't complain about poisoned pages */
	if (PageHWPoison(page)) {
		page_mapcount_reset(page); /* remove PageBuddy */
		return;
	}

	/*
	 * Allow a burst of 60 reports, then keep quiet for that minute;
	 * or allow a steady drip of one report per second.
	 */
	if (nr_shown == 60) {
		if (time_before(jiffies, resume)) {
			nr_unshown++;
			goto out;
		}
		if (nr_unshown) {
			printk(KERN_ALERT
			      "BUG: Bad page state: %lu messages suppressed\n",
				nr_unshown);
			nr_unshown = 0;
		}
		nr_shown = 0;
	}
	if (nr_shown++ == 0)
		resume = jiffies + 60 * HZ;

	printk(KERN_ALERT "BUG: Bad page state in process %s  pfn:%05lx\n",
		current->comm, page_to_pfn(page));
	dump_page(page);

	print_modules();
	dump_stack();
out:
	/* Leave bad fields for debug, except PageBuddy could make trouble */
	page_mapcount_reset(page); /* remove PageBuddy */
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
}

/*
 * Higher-order pages are called "compound pages".  They are structured thusly:
 *
 * The first PAGE_SIZE page is called the "head page".
 *
 * The remaining PAGE_SIZE pages are called "tail pages".
 *
 * All pages have PG_compound set.  All tail pages have their ->first_page
 * pointing at the head page.
 *
 * The first tail page's ->lru.next holds the address of the compound page's
 * put_page() function.  Its ->lru.prev holds the order of allocation.
 * This usage means that zero-order pages may not be compound.
 */

static void free_compound_page(struct page *page)
{
	__free_pages_ok(page, compound_order(page));
}

void prep_compound_page(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;

	set_compound_page_dtor(page, free_compound_page);
	set_compound_order(page, order);
	__SetPageHead(page);
	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;
		set_page_count(p, 0);
		p->first_page = page;
		/* Make sure p->first_page is always valid for PageTail() */
		smp_wmb();
		__SetPageTail(p);
	}
}

/* update __split_huge_page_refcount if you change this function */
static int destroy_compound_page(struct page *page, unsigned long order)
{
	int i;
	int nr_pages = 1 << order;
	int bad = 0;

	if (unlikely(compound_order(page) != order)) {
		bad_page(page);
		bad++;
	}

	__ClearPageHead(page);

	for (i = 1; i < nr_pages; i++) {
		struct page *p = page + i;

		if (unlikely(!PageTail(p) || (p->first_page != page))) {
			bad_page(page);
			bad++;
		}
		__ClearPageTail(p);
	}

	return bad;
}

static inline void prep_zero_page(struct page *page, int order, gfp_t gfp_flags)
{
	int i;

	/*
	 * clear_highpage() will use KM_USER0, so it's a bug to use __GFP_ZERO
	 * and __GFP_HIGHMEM from hard or soft interrupt context.
	 */
	VM_BUG_ON((gfp_flags & __GFP_HIGHMEM) && in_interrupt());
	for (i = 0; i < (1 << order); i++)
		clear_highpage(page + i);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
unsigned int _debug_guardpage_minorder;

static int __init debug_guardpage_minorder_setup(char *buf)
{
	unsigned long res;

	if (kstrtoul(buf, 10, &res) < 0 ||  res > MAX_ORDER / 2) {
		printk(KERN_ERR "Bad debug_guardpage_minorder value\n");
		return 0;
	}
	_debug_guardpage_minorder = res;
	printk(KERN_INFO "Setting debug_guardpage_minorder to %lu\n", res);
	return 0;
}
__setup("debug_guardpage_minorder=", debug_guardpage_minorder_setup);

static inline void set_page_guard_flag(struct page *page)
{
	__set_bit(PAGE_DEBUG_FLAG_GUARD, &page->debug_flags);
}

static inline void clear_page_guard_flag(struct page *page)
{
	__clear_bit(PAGE_DEBUG_FLAG_GUARD, &page->debug_flags);
}
#else
static inline void set_page_guard_flag(struct page *page) { }
static inline void clear_page_guard_flag(struct page *page) { }
#endif

// ARM10C 20140405
// page: 0x20000 (pfn), order: 5
// ARM10C 20140412
// page: 0x20000 (pfn), order: 0
// ARM10C 20140517
// page[16]: order 5의 migratetype MIGRATE_MOVABLE인 lru page + 16, high: 4
static inline void set_page_order(struct page *page, int order)
{
	// [order: 5] page: 0x20000 (pfn), order: 5
	// [order: 0] page: 0x20000 (pfn), order: 0
	set_page_private(page, order);
	// [order: 5] page->private: 5
	// [order: 0] page->private: 0

	// [order: 5] page: 0x20000 (pfn)
	// [order: 0] page: 0x20000 (pfn)
	__SetPageBuddy(page);
	// [order: 5] page->_mapcount: -128
	// [order: 0] page->_mapcount: -128
}

// ARM10C 20140412
// buddy: 0x20000 (pfn)
// ARM10C 20140517
// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
static inline void rmv_page_order(struct page *page)
{
	// page: 0x20000 (pfn)
	__ClearPageBuddy(page);
	// page->_mapcount: -1

	// page: 0x20000 (pfn)
	set_page_private(page, 0);
	// page->private: 0
}

/*
 * Locate the struct page for both the matching buddy in our
 * pair (buddy1) and the combined O(n+1) page they form (page).
 *
 * 1) Any buddy B1 will have an order O twin B2 which satisfies
 * the following equation:
 *     B2 = B1 ^ (1 << O)
 * For example, if the starting buddy (buddy2) is #8 its order
 * 1 buddy is #10:
 *     B2 = 8 ^ (1 << 1) = 8 ^ 2 = 10
 *	(0, 1) (2, 3) (4, 5) (6, 7), (8, 9), (10, 11)
 * 2) Any buddy B will have an order O+1 parent P which
 * satisfies the following equation:
 *     P = B & ~(1 << O)
 *
 * Assumption: *_mem_map is contiguous at least up to MAX_ORDER
 */
// ARM10C 20140405
// page_idx: 0, order: 5
// ARM10C 20140412
// [order: 0] page_idx: 0, order: 0
static inline unsigned long
__find_buddy_index(unsigned long page_idx, unsigned int order)
{
	// [order: 5] page_idx: 0, order: 5
	// [order: 0] page_idx: 0, order: 0
	return page_idx ^ (1 << order);
	// [order: 5] return 32
	// [order: 0] return 1
}

/*
 * This function checks whether a page is free && is the buddy
 * we can do coalesce a page and its buddy if
 * (a) the buddy is not in a hole &&
 * (b) the buddy is in the buddy system &&
 * (c) a page and its buddy have the same order &&
 * (d) a page and its buddy are in the same zone.
 *
 * For recording whether a page is in the buddy system, we set ->_mapcount
 * PAGE_BUDDY_MAPCOUNT_VALUE.
 * Setting, clearing, and testing _mapcount PAGE_BUDDY_MAPCOUNT_VALUE is
 * serialized by zone->lock.
 *
 * For recording page's order, we use page_private(page).
 */
// ARM10C 20140405
// [1] page: 0x20000 (pfn), buddy: 0x20020 (pfn), order: 5
// [2] page: 0x20000 (pfn), buddy: 0x20040 (pfn), order: 6
// ARM10C 20140412
// [order: 0] [1] page: 0x20000 (pfn), buddy: 0x20001 (pfn), order: 0
static inline int page_is_buddy(struct page *page, struct page *buddy,
								int order)
{
	// page_to_pfn(buddy): 0x20020, pfn_valid_within(pfn): 1
	if (!pfn_valid_within(page_to_pfn(buddy)))
		return 0;
	
	// page: 0x20000 (pfn)
	// page_zone_id(page): 0
	// page_zone_id(buddy): 0
	if (page_zone_id(page) != page_zone_id(buddy))
		return 0;

	// page_is_guard(buddy): false
	// [order: 5] [1] page_order(buddy): 0, order: 5
	// [order: 5] [2] page_order(buddy): 0, order: 6
	// [order: 0] [1] page_order(buddy): 0, order: 0
	// [order: 0] [2] page_order(buddy): 0, order: 1
	if (page_is_guard(buddy) && page_order(buddy) == order) {
		VM_BUG_ON(page_count(buddy) != 0);
		return 1;
	}
	
	// [order: 5] [1] PageBuddy(buddy): 0, page_order(buddy): 0, order: 5
	// [order: 5] [2] PageBuddy(buddy): 0, page_order(buddy): 0, order: 6
	// [order: 0] [1] PageBuddy(buddy): 0, page_order(buddy): 0, order: 0
	// [order: 0] [2] PageBuddy(buddy): 0, page_order(buddy): 0, order: 1
	if (PageBuddy(buddy) && page_order(buddy) == order) {
		VM_BUG_ON(page_count(buddy) != 0);
		return 1;
	}
	return 0;
}

/*
 * Freeing function for a buddy system allocator.
 *
 * The concept of a buddy system is to maintain direct-mapped table
 * (containing bit values) for memory blocks of various "orders".
 * The bottom level table contains the map for the smallest allocatable
 * units of memory (here, pages), and each level above it describes
 * pairs of units from the levels below, hence, "buddies".
 * At a high level, all that happens here is marking the table entry
 * at the bottom level available, and propagating the changes upward
 * as necessary, plus some accounting needed to play nicely with other
 * parts of the VM system.
 * At each level, we keep a list of pages, which are heads of continuous
 * free pages of length of (1 << order) and marked with _mapcount
 * PAGE_BUDDY_MAPCOUNT_VALUE. Page's order is recorded in page_private(page)
 * field.
 * So when we are allocating or freeing one, we can derive the state of the
 * other.  That is, if we allocate a small block, and both were
 * free, the remainder of the region must be split into blocks.
 * If a block is freed, and its buddy is also free, then this
 * triggers coalescing into a block of larger size.
 *
 * -- nyc
 */

// ARM10C 20140405
// page: 0x20000(pfn), zone: &contig_page_data->node_zones[ZONE_NORMAL],
// order: 5, migratetype: 0x2
// ARM10C 20140412
// page: 0x20000 (pfn) zone: &(&contig_page_data)->node_zones[ZONE_NORMAL],
// order: 0, mt: 0x2
static inline void __free_one_page(struct page *page,
		struct zone *zone, unsigned int order,
		int migratetype)
{
	unsigned long page_idx;
	unsigned long combined_idx;
	unsigned long uninitialized_var(buddy_idx);
	// unsigned long buddy_idx = buddy_idx 로 변경됨
	// warning 제거용
	struct page *buddy;

	VM_BUG_ON(!zone_is_initialized(zone));

	// PageCompound(page): 0
	if (unlikely(PageCompound(page)))
		if (unlikely(destroy_compound_page(page, order)))
			return;
	
	// migratetype: 2 (MIGRATE_MOVABLE)
	VM_BUG_ON(migratetype == -1);

	// MAX_ORDER: 11, (1 << MAX_ORDER) - 1: 0x7FF
	// page_to_pfn(page): 0x20000
	page_idx = page_to_pfn(page) & ((1 << MAX_ORDER) - 1);
	// page_idx: 0

	// [order: 5] order: 5, (1 << order) - 1: 0x1F
	// [order: 0] order: 0, (1 << order) - 1: 0x1
	VM_BUG_ON(page_idx & ((1 << order) - 1));

	// zone: &contig_page_data->node_zones[ZONE_NORMAL], page: 0x20000(pfn)
	// bad_range(zone, page): 0
	VM_BUG_ON(bad_range(zone, page));

	// MAX_ORDER: 11
	// [order: 5] order  5, MAX_ORDER-1: 10
	// [order: 0] order  0, MAX_ORDER-1: 10
	while (order < MAX_ORDER-1) {

		// [order: 5] page_idx: 0, order: 5
		// [order: 0] page_idx: 0, order: 0
		buddy_idx = __find_buddy_index(page_idx, order);
		// [order: 5] buddy_idx: 32
		// [order: 0] buddy_idx: 1
		// 1 << order 만큼 떨어진 짝의 index를 찾아냄
		
		// [order: 5] page: 0x20000 (pfn), buddy_idx: 32, page_idx: 0
		// [order: 0] page: 0x20000 (pfn), buddy_idx: 1, page_idx: 0
		buddy = page + (buddy_idx - page_idx);
		// [order: 5] buddy: 0x20020 (pfn)
		// [order: 0] buddy: 0x20001 (pfn)

		// [order: 5] page: 0x20000 (pfn), buddy: 0x20020 (pfn), order: 5
		// [order: 5] page_is_buddy(page, buddy, 5): 0
		// [order: 0] page: 0x20000 (pfn), buddy: 0x20001 (pfn), order: 0
		// [order: 0] page_is_buddy(page, buddy, 0): 0
		if (!page_is_buddy(page, buddy, order))
			// 0이 반환됨, loop 빠져나옴
			break;

		// 여기까지 오면 현재 page의 index와 buddy의 index가 같을 경우
		// buddy를 합치는코드 수행

		/*
		 * Our buddy is free or it is CONFIG_DEBUG_PAGEALLOC guard page,
		 * merge with it and move up one order.
		 */
		// node_bootmem_map[0]의 값이 0x000000F0 로 가정하고 분석
		// buddy가 합쳐지는 과정을 분석
		// page: 0x20001 (pfn), buddy: 0x20000 (pfn), order: 0
		//
		// buddy: 0x20000 (pfn)
		// page_is_guard(buddy): false
		if (page_is_guard(buddy)) {
			clear_page_guard_flag(buddy);
			set_page_private(page, 0);
			__mod_zone_freepage_state(zone, 1 << order,
						  migratetype);
		} else {
			// buddy: 0x20000 (pfn)
			list_del(&buddy->lru);
			// contig_page_data.node_zones[ZONE_NORMAL].free_area[0]에
			// 0x20000 (pfn) page을 삭제

			zone->free_area[order].nr_free--;
			// contig_page_data.node_zones[ZONE_NORMAL].free_area[0].nr_free 를
			// 1 만큼 감소

			// buddy: 0x20000 (pfn)
			rmv_page_order(buddy);
			// page->_mapcount: -1
			// page->private: 0
			// 0x20000 (pfn) page 의 _mapcount: -1, private: 0로 설정
		}

		// buddy_idx: 0, page_idx: 1
		combined_idx = buddy_idx & page_idx;
		// combined_idx: 0

		// page: 0x20001 (pfn)
		page = page + (combined_idx - page_idx);
		// page: 0x20000 (pfn)

		// combined_idx: 0
		page_idx = combined_idx;
		// page_idx: 0

		// order: 0
		order++;
		// order: 1
	}
	// [order: 5] page: 0x20000 (pfn), order: 5
	// [order: 0] page: 0x20000 (pfn), order: 0
	set_page_order(page, order);
	// [order: 5] page->private: 5
	// [order: 0] page->private: 0
	// page->_mapcount: -128로 설정

	/*
	 * If this is not the largest possible page, check if the buddy
	 * of the next-highest order is free. If it is, it's possible
	 * that pages are being freed that will coalesce soon. In case,
	 * that is happening, add the free page to the tail of the list
	 * so it's less likely to be used soon and more likely to be merged
	 * as a higher order page
	 */
	// [order: 5] order: 5, MAX_ORDER: 11, page_to_pfn(buddy): 0x20020
	// [order: 0] order: 0, MAX_ORDER: 11, page_to_pfn(buddy): 0x20001
	// pfn_valid_within(1): 1
	if ((order < MAX_ORDER-2) && pfn_valid_within(page_to_pfn(buddy))) {
		struct page *higher_page, *higher_buddy;

		// [order: 5] buddy_idx: 32, page_idx: 0
		// [order: 0] buddy_idx: 1, page_idx: 0
		combined_idx = buddy_idx & page_idx;
		// combined_idx: 0

		// page: 0x20000 (pfn), page_idx: 0
		higher_page = page + (combined_idx - page_idx);
		// higher_page: 0x20000 (pfn)
		// buddy 중에 앞에 것이 반환됨

		// [order: 5] combined_idx: 0, order: 5
		// [order: 0] combined_idx: 0, order: 0
		buddy_idx = __find_buddy_index(combined_idx, order + 1);
		// [order: 5] buddy_idx: 64
		// [order: 0] buddy_idx: 2

		// higher_page: 0x20000 (pfn), combined_idx: 0
		higher_buddy = higher_page + (buddy_idx - combined_idx);
		// [order: 5] higher_buddy : 0x20040 (pfn)
		// [order: 0] higher_buddy : 0x20002 (pfn)

		// [order: 5] higher_page: 0x20000 (pfn), higher_buddy: 0x20040 (pfn), order: 5
		// [order: 0] higher_page: 0x20000 (pfn), higher_buddy: 0x20002 (pfn), order: 0
		if (page_is_buddy(higher_page, higher_buddy, order + 1)) {
			list_add_tail(&page->lru,
				&zone->free_area[order].free_list[migratetype]);
			goto out;
		}
		// page_is_buddy(): 0
	}

	list_add(&page->lru, &zone->free_area[order].free_list[migratetype]);
	// [order: 5] contig_page_data.node_zones[ZONE_NORMAL].free_area[5].free_list[MIGRATE_MOVABLE] 에
	// [order: 0] contig_page_data.node_zones[ZONE_NORMAL].free_area[0].free_list[MIGRATE_MOVABLE] 에
	// 0x20000 (pfn) struct page를 연결
out:
	zone->free_area[order].nr_free++;
	// [order: 5] contig_page_data.node_zones[ZONE_NORMAL].free_area[5].nr_free: 1
	// [order: 5] order 5 인 free 상태의 buddy 갯수를 증가
	// [order: 0] contig_page_data.node_zones[ZONE_NORMAL].free_area[0].nr_free: 1
	// [order: 0] order 0 인 free 상태의 buddy 갯수를 증가
}

// ARM10C 20140405
// page: 0x20000 (pfn)
static inline int free_pages_check(struct page *page)
{
	// page: 0x20000 (pfn), page_mapcount(page): 0,
	// page->mapping: NULL, page->_count: 0,
	// page->flags: 0x20000000, PAGE_FLAGS_CHECK_AT_FREE: 0x18bce1
	// mem_cgroup_bad_page_check(page): false
	if (unlikely(page_mapcount(page) |
		(page->mapping != NULL)  |
		(atomic_read(&page->_count) != 0) |
		(page->flags & PAGE_FLAGS_CHECK_AT_FREE) |
		(mem_cgroup_bad_page_check(page)))) {
		bad_page(page);
		return 1;
	}
	page_cpupid_reset_last(page); // NULL 함수

	// page->flags : 0x20000000
	// PAGE_FLAGS_CHECK_AT_PREP: 0x1FFFFF
	if (page->flags & PAGE_FLAGS_CHECK_AT_PREP)
		page->flags &= ~PAGE_FLAGS_CHECK_AT_PREP;
	// NR_PAGEFLAGS 만큼의 하위 비트를 전부 지워줌

	return 0;
}

/*
 * Frees a number of pages from the PCP lists
 * Assumes all pages on list are in same zone, and of same order.
 * count is the number of pages to free.
 *
 * If the zone was previously in an "all pages pinned" state then look to
 * see if this freeing clears that state.
 *
 * And clear the zone's pages_scanned counter, to hold off the "all pages are
 * pinned" detection logic.
 */
// ARM10C 20140412
// zone: &(&contig_page_data)->node_zones[ZONE_NORMAL], batch: 1,
// pcp: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp
static void free_pcppages_bulk(struct zone *zone, int count,
					struct per_cpu_pages *pcp)
{
	int migratetype = 0;
	int batch_free = 0;
	// count: 1
	int to_free = count;
	// to_free: 1

	// &zone->lock: &(&contig_page_data)->node_zones[ZONE_NORMAL].lock
	spin_lock(&zone->lock);
	// zone 의 spin lock 획득

	zone->pages_scanned = 0;
	// zone->pages_scanned : (&contig_page_data)->node_zones[ZONE_NORMAL].pages_scanned: 0

	// to_free: 1
	while (to_free) {
		struct page *page;
		struct list_head *list;

		/*
		 * Remove pages from lists in a round-robin fashion. A
		 * batch_free count is maintained that is incremented when an
		 * empty list is encountered.  This is so more pages are freed
		 * off fuller lists instead of spinning excessively around empty
		 * lists
		 */
		do {
			// [1st] batch_free: 0
			// [2nd] batch_free: 1
			batch_free++;
			// [1st] batch_free: 1
			// [2nd] batch_free: 2

			// [1st] migratetype: 1, MIGRATE_PCPTYPES: 3
			// [2nd] migratetype: 2, MIGRATE_PCPTYPES: 3
			if (++migratetype == MIGRATE_PCPTYPES)
				migratetype = 0;
			// [1st] migratetype: 1
			// [2nd] migratetype: 2

			// [1st] &pcp->lists: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists
			// [2nd] &pcp->lists: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists
			list = &pcp->lists[migratetype];
			// [1st] list: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists[1]
			// [2nd] list: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists[2]
		} while (list_empty(list));
		// [2nd]에서 &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists[2]에 page->lru 연결
		// 되었으므로 빠져나옴

		/* This is the only non-empty list. Free them all. */
		// batch_free: 2, MIGRATE_PCPTYPES: 3
		if (batch_free == MIGRATE_PCPTYPES)
			batch_free = to_free;

		do {
			int mt;	/* migratetype of the to-be-freed page */

			// list->prev: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists[2].prev
			page = list_entry(list->prev, struct page, lru);
			// page: 0x20000 (pfn)

			/* must delete as __free_one_page list manipulates */
			// page: 0x20000 (pfn)
			list_del(&page->lru);
			// pcp list에 추가된 0x20000 (pfn) page lru 삭제

			// # list_del(lru)가 의미하는 바
			// pcp list에 page->lru를 붙인다는 것은 가장 최근에 수정된 page와 연결된
			// pcp list의 위치의 뒤에 붙여서 일종의 꼬리표처럼 된다.
			// 즉 page->lru를 찾으면 가장 최근에 수정된 page의 위치를 알수 있게 된다.
			// page->lru를 찾고 나서 page->lru를 지우면 최근에 수정된 page의 위치를 참조하게된다.

			// page: 0x20000 (pfn)
			mt = get_freepage_migratetype(page);
			// mt: 0x2
			// page->index값 추출

			/* MIGRATE_MOVABLE list may include MIGRATE_RESERVEs */
			// page: 0x20000 (pfn)
			// zone: &(&contig_page_data)->node_zones[ZONE_NORMAL], mt: 0x2
			__free_one_page(page, zone, 0, mt);
			// contig_page_data.node_zones[ZONE_NORMAL].free_area[0].free_list[MIGRATE_MOVABLE] 에
			// 0x20000 (pfn) struct page를 연결
			// contig_page_data.node_zones[ZONE_NORMAL].free_area[0].nr_free
			// order 0 인 free 상태의 buddy 갯수를 증가

			// page: 0x20000 (pfn), mt: 0x2
			trace_mm_page_pcpu_drain(page, 0, mt);

			// page: 0x20000 (pfn), is_migrate_isolate_page(page): false
			if (likely(!is_migrate_isolate_page(page))) {
				// zone: &(&contig_page_data)->node_zones[ZONE_NORMAL], NR_FREE_PAGES: 0
				__mod_zone_page_state(zone, NR_FREE_PAGES, 1);
				// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[NR_FREE_PAGES]: 1 을 더함
				// vmstat.c의 vm_stat[NR_FREE_PAGES] 전역 변수에도 1을 더함

				// mt: 0x2, is_migrate_cma(0x2): 0
				if (is_migrate_cma(mt))
					__mod_zone_page_state(zone, NR_FREE_CMA_PAGES, 1);
			}
		// to_free: 0, batch_free: 2
		} while (--to_free && --batch_free && !list_empty(list));
	}

	spin_unlock(&zone->lock);
	// zone 의 spin lock 해제
}

// ARM10C 20140405
// zone: &contig_page_data->node_zones[ZONE_NORMAL], page: 0x20000(pfn)
// order: 5, migratetype: 0x2
static void free_one_page(struct zone *zone, struct page *page, int order,
				int migratetype)
{
	spin_lock(&zone->lock);
	// 스핀락 획득

	zone->pages_scanned = 0;

	// page: 0x20000(pfn), zone: &contig_page_data->node_zones[ZONE_NORMAL],
	// order: 5, migratetype: 0x2
	__free_one_page(page, zone, order, migratetype);
	// order 5 buddy를 contig_page_data에 추가함

// 2014/04/05 종료
// 2014/04/12 시작

	// is_migrate_isolate(migratetype): false
	if (unlikely(!is_migrate_isolate(migratetype)))
		// zone: &contig_page_data->node_zones[ZONE_NORMAL], order: 5, migratetype: 0x2
		__mod_zone_freepage_state(zone, 1 << order, migratetype);

	spin_unlock(&zone->lock);
	// 스핀락 해제
}

// ARM10C 20140329
// page: 0x20000의 해당하는 struct page의 1st page, order: 5
// ARM10C 20140412
// page: 0x20000 (pfn), order: 0
static bool free_pages_prepare(struct page *page, unsigned int order)
{
	int i;
	int bad = 0;

	// mm_page_free이름으로 아래의 trace 관련 함수가 만들어져 있음
	// trace_mm_page_free, register_trace_mm_page_free
	// unregister_trace_mm_page_free, check_trace_callback_type_mm_page_free

	// page: 0x20000의 해당하는 struct page의 1st page, order: 5
	trace_mm_page_free(page, order);

	// page: 0x20000의 해당하는 struct page의 1st page, order: 5
	kmemcheck_free_shadow(page, order); // null function

	// page: 0x20000의 해당하는 struct page의 1st page
	// PageAnon(page): 0
	if (PageAnon(page))
		page->mapping = NULL;

// 2014/03/29 종료
// 2014/04/05 시작

	// [order: 5] order: 5
	// [order: 0] order: 0
	for (i = 0; i < (1 << order); i++)
		// page: 0x20000 (pfn)
		bad += free_pages_check(page + i);
		// page->flags의 NR_PAGEFLAGS 만큼의 하위 비트를 전부 지워줌
		// page가 free 된 것이면 0 반환

	if (bad)
		return false;

	// PageHighMem(page): 0
	if (!PageHighMem(page)) {
		debug_check_no_locks_freed(page_address(page),
					   PAGE_SIZE << order); // null function
		debug_check_no_obj_freed(page_address(page),
					   PAGE_SIZE << order); // null function
	}

	// page: 0x20000 (pfn), order: 5
	arch_free_page(page, order); // null function

	// page: 0x20000 (pfn), 32, 0
	kernel_map_pages(page, 1 << order, 0); // null function

	return true;
}

// ARM10C 20140329
// page: 0x20000의 해당하는 struct page의 1st page, order: 5
static void __free_pages_ok(struct page *page, unsigned int order)
{
	unsigned long flags;
	int migratetype;

	// page: 0x20000의 해당하는 struct page의 1st page, order: 5
	// free_pages_prepare(page, 5): true
	if (!free_pages_prepare(page, order))
		return;
	// page->flags의 NR_PAGEFLAGS 만큼의 하위 비트를 전부 지워줌


	local_irq_save(flags);
	// flags에 cpsr 저장

	// PGFREE: 7, order: 5
	__count_vm_events(PGFREE, 1 << order);
	// CPU0의 vm_event_states.event[PGFREE] 를 32로 설정함

	migratetype = get_pageblock_migratetype(page);
	// migratetype: 0x2
	// page에 해당하는 pageblock의 migrate flag를 반환함

	set_freepage_migratetype(page, migratetype);
	// struct page의 index 멤버에 migratetype을 저장함

	// page: 0x20000 (pfn), page_zone(page): &contig_page_data->node_zones[ZONE_NORMAL]
	// order: 5, migratetype: 0x2
	free_one_page(page_zone(page), page, order, migratetype);
	// order 5 buddy를 contig_page_data에 추가함
	// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[NR_FREE_PAGES]: 32 로 설정
	// vmstat.c의 vm_stat[NR_FREE_PAGES] 전역 변수에도 32로 설정

	local_irq_restore(flags);
	// flags에 저장된 cpsr을 복원
}

// ARM10C 20140329
// pfn_to_page(0x20000): 0x20000의 해당하는 struct page의 주소, order: 5
// ARM10C 20140412
// [order: 0] page: 0x20000 (pfn), 0
void __init __free_pages_bootmem(struct page *page, unsigned int order)
{
	// order: 5
	// [order: 0] order: 0
	unsigned int nr_pages = 1 << order;
	// nr_pages: 32
	// [order: 0] nr_pages: 1
	struct page *p = page;
	unsigned int loop;

	// page: 0x20000의 해당하는 struct page의 주소
	prefetchw(p);
	// cache에 0x20000의 해당하는 struct page를 읽음

	// nr_pages: 32
	// [order: 0] nr_pages: 1
	for (loop = 0; loop < (nr_pages - 1); loop++, p++) {
		// p: 0x20000 (pfn)
		// p: 0x20001 (pfn)
		prefetchw(p + 1);
		// 0x20001 (pfn) 을 cache에 추가
		// 0x20002 (pfn) 을 cache에 추가

		// p: 0x20000 (pfn)
		// p: 0x20001 (pfn)
		__ClearPageReserved(p);
		// page reserved 속성을 clear

		// p: 0x20000 (pfn)
		// p: 0x20001 (pfn)
		set_page_count(p, 0);
		// p: 0x20000 (pfn) page count를 0으로 초기화
		// p: 0x20001 (pfn) page count를 0으로 초기화

		// ... loop: 2~31은 생략
	}
	__ClearPageReserved(p);
	set_page_count(p, 0);

	// page: 0x20000의 해당하는 struct page의 주소
	// page_zone(page)->managed_pages:
	// (&contig_page_data)->node_zones[page_zonenum(page)].managed_pages: 0
	// [order: 5] page: 0x20000 (pfn), order: 5
	// [order: 0] page: 0x20000 (pfn), order: 0
	page_zone(page)->managed_pages += nr_pages;
	// [order: 5] page_zone(page)->managed_pages:
	// [order: 5] (&contig_page_data)->node_zones[page_zonenum(page)].managed_pages: 32
	// [order: 0] page_zone(page)->managed_pages:
	// [order: 0] (&contig_page_data)->node_zones[page_zonenum(page)].managed_pages: 1

	// page: 0x20000의 해당하는 struct page의 주소
	set_page_refcounted(page);
	// page: 0x20000의 해당하는 struct page의 1st page의 _count를 1로 set

	// [order: 5] order: 5
	// [order: 0] order: 0
	__free_pages(page, order);
	// [order: 5] CPU0의 vm_event_states.event[PGFREE] 를 32로 설정함
	// [order: 5] page에 해당하는 pageblock의 migrate flag를 반환함
	// [order: 5] struct page의 index 멤버에 migratetype을 저장함
	// [order: 5] order 5 buddy를 contig_page_data에 추가함
	// [order: 5] (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[NR_FREE_PAGES]: 32 로 설정
	// [order: 5] vmstat.c의 vm_stat[NR_FREE_PAGES] 전역 변수에도 32로 설정
	// [order: 0] CPU0의 vm_event_states.event[PGFREE] 를 1로 설정함
	// [order: 0] page에 해당하는 pageblock의 migrate flag를 반환함
	// [order: 0] struct page의 index 멤버에 migratetype을 저장함
	// [order: 0] order 0 buddy를 contig_page_data에 추가함
	// [order: 0] (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[NR_FREE_PAGES]: 1 로 설정
	// [order: 0] vmstat.c의 vm_stat[NR_FREE_PAGES] 전역 변수에도 1로 설정
}

#ifdef CONFIG_CMA
/* Free whole pageblock and set its migration type to MIGRATE_CMA. */
void __init init_cma_reserved_pageblock(struct page *page)
{
	unsigned i = pageblock_nr_pages;
	struct page *p = page;

	do {
		__ClearPageReserved(p);
		set_page_count(p, 0);
	} while (++p, --i);

	set_page_refcounted(page);
	set_pageblock_migratetype(page, MIGRATE_CMA);
	__free_pages(page, pageblock_order);
	adjust_managed_page_count(page, pageblock_nr_pages);
}
#endif

/*
 * The order of subdivision here is critical for the IO subsystem.
 * Please do not alter this order without good reasons and regression
 * testing. Specifically, as large blocks of memory are subdivided,
 * the order in which smaller blocks are delivered depends on the order
 * they're subdivided in this function. This is the primary factor
 * influencing the order in which pages are delivered to the IO
 * subsystem according to empirical testing, and this is also justified
 * by considering the behavior of a buddy system containing a single
 * large block of memory acted on by a series of small allocations.
 * This behavior is a critical factor in sglist merging's success.
 *
 * -- nyc
 */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0], page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
// order: 0, current_order: 5, area: &contig_page_data->node_zones[0].free_area[5], new_type: 0
static inline void expand(struct zone *zone, struct page *page,
	int low, int high, struct free_area *area,
	int migratetype)
{
	// high: 5
	unsigned long size = 1 << high;
	// size: 32

	// high: 5, low: 0
	while (high > low) {
		// area: &contig_page_data->node_zones[0].free_area[5]
		area--;
		// area: &contig_page_data->node_zones[0].free_area[4]

		// high: 5
		high--;
		// high: 4

		// size: 32
		size >>= 1;
		// size: 16

		// zone: contig_page_data->node_zones[0],
		// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page,
		// bad_range(contig_page_data->node_zones[0],
		//	    order 5의 migratetype MIGRATE_MOVABLE인 lru page + 16): 0
		VM_BUG_ON(bad_range(zone, &page[size]));

#ifdef CONFIG_DEBUG_PAGEALLOC // CONFIG_DEBUG_PAGEALLOC=n
		if (high < debug_guardpage_minorder()) {
			/*
			 * Mark as guard pages (or page), that will allow to
			 * merge back to allocator when buddy will be freed.
			 * Corresponding page table entries will not be touched,
			 * pages will stay not present in virtual address space
			 */
			INIT_LIST_HEAD(&page[size].lru);
			set_page_guard_flag(&page[size]);
			set_page_private(&page[size], high);
			/* Guard pages are not available for any usage */
			__mod_zone_freepage_state(zone, -(1 << high),
						  migratetype);
			continue;
		}
#endif
		// size: 16, page[16].lru: (order 5의 migratetype MIGRATE_MOVABLE인 lru page + 16).lru
		// migratetype: 0, area->free_list[0]: (&contig_page_data->node_zones[0].free_area[4])->free_list[0]
		list_add(&page[size].lru, &area->free_list[migratetype]);
		// (&contig_page_data->node_zones[0].free_area[4])->free_list[0] 에
		// order 5의 migratetype MIGRATE_MOVABLE인 lru page + 16 인 page 추가

		// area->nr_free: (&contig_page_data->node_zones[0].free_area[4])->nr_free
		area->nr_free++;
		// free page 수 증가

		// size: 16, page[16]: order 5의 migratetype MIGRATE_MOVABLE인 lru page + 16, high: 4
		set_page_order(&page[size], high);
		// page의 _mapcount 값을 -128, private를 4로 변경
	}
}

/*
 * This page is about to be returned from the page allocator
*/
// ARM10C 20140524
// p: migratetype이 MIGRATE_UNMOVABLE인 page
static inline int check_new_page(struct page *page)
{
	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	// PAGE_FLAGS_CHECK_AT_PREP: 0x1FFFFF
	// mem_cgroup_bad_page_check(page): false
	// page_mapcount(page): 0
	// page->mapping: null, page->_count: 0
	if (unlikely(page_mapcount(page) |
		(page->mapping != NULL)  |
		(atomic_read(&page->_count) != 0)  |
		(page->flags & PAGE_FLAGS_CHECK_AT_PREP) |
		(mem_cgroup_bad_page_check(page)))) {
		bad_page(page);
		return 1;
	}

	// page->_mapcount: -1: 사용하기위해 할당 받았지만 쓰지는 않음
	// page->mapping: 할당 받았으면 null 로 됨, 사용중인 page는 vma, inode 등의
	//                address space로 맵핑된다
	// page->_count: 현재 page 를 참조한 숫자값

	return 0;
}

// ARM10C 20140524
// page: migratetype이 MIGRATE_UNMOVABLE인 page, order: 0, gfp_flags: 0x221200
static int prep_new_page(struct page *page, int order, gfp_t gfp_flags)
{
	int i;

	// order: 0
	for (i = 0; i < (1 << order); i++) {
		// page: migratetype이 MIGRATE_UNMOVABLE인 page
		struct page *p = page + i;
		// p: migratetype이 MIGRATE_UNMOVABLE인 page

		// check_new_page(migratetype이 MIGRATE_UNMOVABLE인 page): 0
		if (unlikely(check_new_page(p)))
			return 1;
	}

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	set_page_private(page, 0);
	// page->private: 0
	// FIXME: 왜 pamam order값을 보지 않고 0으로 초기화 하는지?

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	set_page_refcounted(page);
	// page->_count를 1로 set

	// page: migratetype이 MIGRATE_UNMOVABLE인 page, order: 0
	arch_alloc_page(page, order); // null function

	// page: migratetype이 MIGRATE_UNMOVABLE인 page, order: 0
	kernel_map_pages(page, 1 << order, 1); // null function

	// gfp_flags: 0x221200, __GFP_ZERO: 0x8000u
	if (gfp_flags & __GFP_ZERO)
		prep_zero_page(page, order, gfp_flags);

	// order: 0, gfp_flags: 0x221200, __GFP_COMP: 0x4000u
	if (order && (gfp_flags & __GFP_COMP))
		prep_compound_page(page, order);

	return 0;
}

/*
 * Go through the free lists for the given migratetype and remove
 * the smallest available page from the freelists
 */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0], order: 0, migratetype: MIGRATE_UNMOVABLE: 0
static inline
struct page *__rmqueue_smallest(struct zone *zone, unsigned int order,
						int migratetype)
{
	unsigned int current_order;
	struct free_area *area;
	struct page *page;

	/* Find a page of the appropriate size in the preferred list */
	// order: 0, MAX_ORDER: 11
	for (current_order = order; current_order < MAX_ORDER; ++current_order) {
		// current_order: 0
		// zone->free_area[0]: contig_page_data->node_zones[0].free_area[0]
		area = &(zone->free_area[current_order]);
		// area: &(contig_page_data->node_zones[0].free_area[0])

		// migratetype: 0,
		// area->free_list[0]: (&(contig_page_data->node_zones[0].free_area[0]))->free_list[0]
		// list_empty((&(contig_page_data->node_zones[0].free_area[0]))->free_list[0]): 1
		if (list_empty(&area->free_list[migratetype]))
			continue;

		page = list_entry(area->free_list[migratetype].next,
							struct page, lru);
		list_del(&page->lru);
		rmv_page_order(page);
		area->nr_free--;
		expand(zone, page, order, current_order, area, migratetype);
		// expand 의 동작:
		// order의 의미: 사용하고자 하는 크기, current_order의 의미: buddy에서 가져온 크기
		// 큰 buddy order 에서 사용하고자 하는 order를 제외한 나머지 buddy 영역을 free 상태로 정리

		return page;
	}

	return NULL;
	// return NULL
}


/*
 * This array describes the order lists are fallen back to when
 * the free lists for the desirable migrate type are depleted
 */
// ARM10C 20140517
// MIGRATE_TYPES: 4
static int fallbacks[MIGRATE_TYPES][4] = {
	[MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE,     MIGRATE_RESERVE },
	[MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE,     MIGRATE_RESERVE },
#ifdef CONFIG_CMA // CONFIG_CMA=n
	[MIGRATE_MOVABLE]     = { MIGRATE_CMA,         MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE, MIGRATE_RESERVE },
	[MIGRATE_CMA]         = { MIGRATE_RESERVE }, /* Never used */
#else
	[MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE,   MIGRATE_RESERVE },
#endif
	[MIGRATE_RESERVE]     = { MIGRATE_RESERVE }, /* Never used */
#ifdef CONFIG_MEMORY_ISOLATION // CONFIG_MEMORY_ISOLATION=n
	[MIGRATE_ISOLATE]     = { MIGRATE_RESERVE }, /* Never used */
#endif
};

/*
 * Move the free pages in a range to the free lists of the requested type.
 * Note that start_page and end_pages are not aligned on a pageblock
 * boundary. If alignment is required, use move_freepages_block()
 */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0], start_page: page 0x20000 (pfn),
// end_page: page 0x20cff (pfn), migratetype: 0
int move_freepages(struct zone *zone,
			  struct page *start_page, struct page *end_page,
			  int migratetype)
{
	struct page *page;
	unsigned long order;
	int pages_moved = 0;

#ifndef CONFIG_HOLES_IN_ZONE // CONFIG_HOLES_IN_ZONE=n
	/*
	 * page_zone is not safe to call in this context when
	 * CONFIG_HOLES_IN_ZONE is set. This bug check is probably redundant
	 * anyway as we check zone boundaries in move_freepages_block().
	 * Remove at a later date when no bug reports exist related to
	 * grouping pages by mobility
	 */
	// start_page: 0x20000 (pfn), end_page: 0x20cff (pfn)
	BUG_ON(page_zone(start_page) != page_zone(end_page));
#endif

	// start_page: page 0x20000 (pfn), end_page: page 0x20cff (pfn)
	for (page = start_page; page <= end_page;) {
		/* Make sure we are not inadvertently changing nodes */
		// page: 0x20000 (pfn), zone: contig_page_data->node_zones[0]
		VM_BUG_ON(page_to_nid(page) != zone_to_nid(zone));

		// page: 0x20000 (pfn), pfn_valid_within(0x20000): 1
		if (!pfn_valid_within(page_to_pfn(page))) {
			page++;
			continue;
		}

		// page: 0x20000 (pfn), PageBuddy(0x20000): 1
		if (!PageBuddy(page)) {
			page++;
			continue;
		}

		// page: 0x20000 (pfn)
		order = page_order(page);
		// order: 5

		// zone->free_area[5].free_list[0]: contig_page_data->node_zones[0].free_area[5].free_list[0]
		list_move(&page->lru,
			  &zone->free_area[order].free_list[migratetype]);
		// contig_page_data->node_zones[0].free_area[5].free_list[2]
		// 에서 contig_page_data->node_zones[0].free_area[5].free_list[0] 로 옮김

		// page: 0x20000 (pfn), migratetype: 0
		set_freepage_migratetype(page, migratetype);
		// migratetype이 2에서 0으로 변경됨

		// order: 5
		page += 1 << order;
		// page: 0x20020 (pfn)

		// pages_moved: 0
		pages_moved += 1 << order;
		// pages_moved: 32
	}

	// pages_moved: ??? (옮겨진 page 갯수)
	return pages_moved;
}

// ARM10C 20140517
// zone: contig_page_data->node_zones[0],
// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
// start_type: 0
int move_freepages_block(struct zone *zone, struct page *page,
				int migratetype)
{
	unsigned long start_pfn, end_pfn;
	struct page *start_page, *end_page;

	// 가정: order 5의 migratetype MIGRATE_MOVABLE인 lru page
	//       의 pfn값을 0x20000으로 가정하고 분석

	// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
	start_pfn = page_to_pfn(page);
	// start_pfn: 0x20000

	// pageblock_nr_pages : 0x400
	start_pfn = start_pfn & ~(pageblock_nr_pages-1);
	// start_pfn: 0x20000

	start_page = pfn_to_page(start_pfn);
	// start_page : page 0x20000 (pfn)

	// pageblock_nr_pages: 0x400
	end_page = start_page + pageblock_nr_pages - 1;
	// end_page: page 0x20cff (pfn)

	// start_pfn: 0x20000, pageblock_nr_pages: 0x400
	end_pfn = start_pfn + pageblock_nr_pages - 1;
	// end_pfn: 0x20cff

	/* Do not cross zone boundaries */
	// zone: contig_page_data->node_zones[0], start_pfn: 0x20000
	// zone_spans_pfn(contig_page_data->node_zones[0], 0x20000): 1
	if (!zone_spans_pfn(zone, start_pfn))
		start_page = page;

	// zone: contig_page_data->node_zones[0], end_pfn: 0x20cff
	// zone_spans_pfn(contig_page_data->node_zones[0], 0x20cff): 1
	if (!zone_spans_pfn(zone, end_pfn))
		return 0;

	// zone: contig_page_data->node_zones[0], start_page: page 0x20000 (pfn),
	// end_page: page 0x20cff (pfn), migratetype: 0
	return move_freepages(zone, start_page, end_page, migratetype);
	// pages_moved: ??? (옮겨진 page 갯수)
}

static void change_pageblock_range(struct page *pageblock_page,
					int start_order, int migratetype)
{
	int nr_pageblocks = 1 << (start_order - pageblock_order);

	while (nr_pageblocks--) {
		set_pageblock_migratetype(pageblock_page, migratetype);
		pageblock_page += pageblock_nr_pages;
	}
}

/*
 * If breaking a large block of pages, move all free pages to the preferred
 * allocation list. If falling back for a reclaimable kernel allocation, be
 * more aggressive about taking ownership of free pages.
 *
 * On the other hand, never change migration type of MIGRATE_CMA pageblocks
 * nor move CMA pages to different free lists. We don't want unmovable pages
 * to be allocated from MIGRATE_CMA areas.
 *
 * Returns the new migratetype of the pageblock (or the same old migratetype
 * if it was unchanged).
 */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0],
// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
// start_migratetype: 0
// migratetype: MIGRATE_MOVABLE: 2
static int try_to_steal_freepages(struct zone *zone, struct page *page,
				  int start_type, int fallback_type)
{
	// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
	int current_order = page_order(page);
	// current_order: 5

	/*
	 * When borrowing from MIGRATE_CMA, we need to release the excess
	 * buddy pages to CMA itself.
	 */
	// fallback_type: MIGRATE_MOVABLE: 2
	// is_migrate_cma(2): false
	if (is_migrate_cma(fallback_type))
		return fallback_type;

	/* Take ownership for orders >= pageblock_order */
	// current_order: 5, pageblock_order: 10
	if (current_order >= pageblock_order) {
		change_pageblock_range(page, current_order, start_type);
		return start_type;
	}

	// current_order: 5, pageblock_order: 10, start_type: 0,
	// MIGRATE_RECLAIMABLE: 1, page_group_by_mobility_disabled: 0
	if (current_order >= pageblock_order / 2 ||
	    start_type == MIGRATE_RECLAIMABLE ||
	    page_group_by_mobility_disabled) {
		int pages;

		// zone: contig_page_data->node_zones[0],
		// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
		// start_type: 0
		pages = move_freepages_block(zone, page, start_type);
		// pages: ??? (옮겨진 page 갯수)

		/* Claim the whole block if over half of it is free */
		// pages: ??? (옮겨진 page 갯수), pageblock_order: 10
		// page_group_by_mobility_disabled: 0
		if (pages >= (1 << (pageblock_order-1)) ||
				page_group_by_mobility_disabled) {

			// pages: ??? (옮겨진 page 갯수) 512개 이상임
			// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
			// start_type: 0
			set_pageblock_migratetype(page, start_type);
			// page block의 migratetype을 2에서 0으로 변경

			// start_type: 0
			return start_type;
			// return 0
		}

	}

	return fallback_type;
}

/* Remove an element from the buddy allocator from the fallback list */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0], order: 0, migratetype: MIGRATE_UNMOVABLE: 0
static inline struct page *
__rmqueue_fallback(struct zone *zone, int order, int start_migratetype)
{
	struct free_area *area;
	int current_order;
	struct page *page;
	int migratetype, new_type, i;

	/* Find the largest possible block of pages in the other list */
	// MAX_ORDER: 11, order: 0
	for (current_order = MAX_ORDER-1; current_order >= order;
						--current_order) {
		for (i = 0;; i++) {
			// i: 0, start_migratetype: 0, fallbacks[0][0]: MIGRATE_RECLAIMABLE: 1
			// i: 1, start_migratetype: 0, fallbacks[0][1]: MIGRATE_MOVABLE: 2
			migratetype = fallbacks[start_migratetype][i];
			// i: 0, migratetype: MIGRATE_RECLAIMABLE: 1
			// i: 1, migratetype: MIGRATE_MOVABLE: 2

			/* MIGRATE_RESERVE handled later if necessary */
			// i: 0, migratetype: MIGRATE_RECLAIMABLE: 1
			// i: 1, migratetype: MIGRATE_MOVABLE: 2
			if (migratetype == MIGRATE_RESERVE)
				break;

			// i: 0, current_order: 10
			// i: 0, zone->free_area[10]: contig_page_data->node_zones[0].free_area[10]
			// i: 1, current_order: 10
			// i: 1, zone->free_area[10]: contig_page_data->node_zones[0].free_area[10]
			area = &(zone->free_area[current_order]);
			// i: 0, area: &contig_page_data->node_zones[0].free_area[10]
			// i: 1, area: &contig_page_data->node_zones[0].free_area[10]

			// i: 0, migratetype: MIGRATE_RECLAIMABLE: 1
			// i: 0, area->free_list[1]: (&contig_page_data->node_zones[0].free_area[10])->free_list[1]
			// i: 1, migratetype: MIGRATE_MOVABLE: 2
			// i: 1, area->free_list[2]: (&contig_page_data->node_zones[0].free_area[10])->free_list[2]
			if (list_empty(&area->free_list[migratetype]))
				continue;

			// 가정: current_order: 5, migratetype: MIGRATE_MOVABLE: 2 일때 아래 코드 수행이
			//       될것이라 보고 분석

			// i: 1, current_order: 5
			// i: 1, migratetype: MIGRATE_MOVABLE: 2
			// i: 1, area->free_list[2].next: (&contig_page_data->node_zones[0].free_area[5])->free_list[2].next
			page = list_entry(area->free_list[migratetype].next,
					struct page, lru);
			// page: order 5의 migratetype MIGRATE_MOVABLE인 lru page

			// i: 1, area->nr_free: (&contig_page_data->node_zones[0].free_area[5])->nr_free
			area->nr_free--;
			// area->nr_free: (&contig_page_data->node_zones[0].free_area[5])->nr_free 1 감소

			// i: 1, zone: contig_page_data->node_zones[0],
			// i: 1, page: order 5의 migratetype MIGRATE_MOVABLE인 lru page,
			// i: 1, start_migratetype: 0,
			// i: 1, migratetype: MIGRATE_MOVABLE: 2,
			// i: 1, try_to_steal_freepages(contig_page_data->node_zones[0],
			//				order 5의 migratetype MIGRATE_MOVABLE인 lru page, 0, 2): 0
			new_type = try_to_steal_freepages(zone, page,
							  start_migratetype,
							  migratetype);
			// i: 1, new_type: 0

			/* Remove the page from the freelists */
			// i: 1, page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
			list_del(&page->lru);

			// i: 1, page: order 5의 migratetype MIGRATE_MOVABLE인 lru page
			rmv_page_order(page);
			// i: 1, page의 _mapcount를 -1, private 값을 0으로 초기화

			// i: 1, zone: contig_page_data->node_zones[0],
			// i: 1, page: order 5의 migratetype MIGRATE_MOVABLE인 lru page,
			// i: 1, order: 0, current_order: 5, area: &contig_page_data->node_zones[0].free_area[5],
			// i: 1, new_type: 0
			expand(zone, page, order, current_order, area,
			       new_type);
			// expand 의 동작:
			// order의 의미: 사용하고자 하는 크기, current_order의 의미: buddy에서 가져온 크기
			// 큰 buddy order 에서 사용하고자 하는 order를 제외한 나머지 buddy 영역을 free 상태로 정리

			trace_mm_page_alloc_extfrag(page, order, current_order,
				start_migratetype, migratetype, new_type);

			// page: migratetype이 MIGRATE_UNMOVABLE인 page
			return page;
		}
	}

	return NULL;
}

/*
 * Do the hard work of removing an element from the buddy allocator.
 * Call me with the zone->lock already held.
 */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0], order: 0, migratetype: MIGRATE_UNMOVABLE: 0
static struct page *__rmqueue(struct zone *zone, unsigned int order,
						int migratetype)
{
	struct page *page;

retry_reserve:
	// zone: contig_page_data->node_zones[0], order: 0, migratetype: MIGRATE_UNMOVABLE: 0
	// __rmqueue_smallest(contig_page_data->node_zones[0], 0, 0): NULL
	page = __rmqueue_smallest(zone, order, migratetype);
	// page: NULL

	// page: NULL, migratetype: 0, MIGRATE_RESERVE: 3
	if (unlikely(!page) && migratetype != MIGRATE_RESERVE) {
		// zone: contig_page_data->node_zones[0], order: 0, migratetype: MIGRATE_UNMOVABLE: 0
		page = __rmqueue_fallback(zone, order, migratetype);
		// page: migratetype이 MIGRATE_UNMOVABLE인 page

		/*
		 * Use MIGRATE_RESERVE rather than fail an allocation. goto
		 * is used because __rmqueue_smallest is an inline function
		 * and we want just one call site
		 */
		// page: migratetype이 MIGRATE_UNMOVABLE인 page
		if (!page) {
			migratetype = MIGRATE_RESERVE;
			goto retry_reserve;
		}
	}

	trace_mm_page_alloc_zone_locked(page, order, migratetype);

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	return page;
}

/*
 * Obtain a specified number of elements from the buddy allocator, all under
 * a single hold of the lock, for efficiency.  Add them to the supplied list.
 * Returns the number of new pages which were placed at *list.
 */
// ARM10C 20140517
// zone: contig_page_data->node_zones[0], 0,
// pcp->batch: (&boot_pageset + (__per_cpu_offset[0]))->pcp.batch: 1
// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
// migratetype: MIGRATE_UNMOVABLE: 0, cold: 0
static int rmqueue_bulk(struct zone *zone, unsigned int order,
			unsigned long count, struct list_head *list,
			int migratetype, int cold)
{
	// migratetype: MIGRATE_UNMOVABLE: 0
	int mt = migratetype, i;
	// mt: 0

	// zone->lock: contig_page_data->node_zones[0].lock
	spin_lock(&zone->lock);
	// spinlock 획득

	// count: 1
	for (i = 0; i < count; ++i) {
		// zone: contig_page_data->node_zones[0], order: 0, migratetype: MIGRATE_UNMOVABLE: 0
		struct page *page = __rmqueue(zone, order, migratetype);
		// page: migratetype이 MIGRATE_UNMOVABLE인 page

		// __rmqueue에서 한일:
		// 해당 zone에서 order에 맞고 migratetype이 같은 page를 구해옴
		// migratetype 이 일치 하지 않으면
		// fallbacks의 enum의 메모리 정책에 의해 할당받는 순서가 결정됨

		// page: migratetype이 MIGRATE_UNMOVABLE인 page
		if (unlikely(page == NULL))
			break;

		/*
		 * Split buddy pages returned by expand() are received here
		 * in physical page order. The page is added to the callers and
		 * list and the list head then moves forward. From the callers
		 * perspective, the linked list is ordered by page number in
		 * some conditions. This is useful for IO devices that can
		 * merge IO requests if the physical pages are ordered
		 * properly.
		 */
		// cold: 0
		if (likely(cold == 0))
			// page->lru: (migratetype이 MIGRATE_UNMOVABLE인 page)->lru
			// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
			list_add(&page->lru, list);
			// (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]에
			// (migratetype이 MIGRATE_UNMOVABLE인 page)->lru가 head로 등록
		else
			list_add_tail(&page->lru, list);

		// page의 lru는 cold가 1일때가 tail로 cold가 0일때 head로 등록됨

		// CONFIG_CMA=n, IS_ENABLED(CONFIG_CMA): 0
		if (IS_ENABLED(CONFIG_CMA)) {
			mt = get_pageblock_migratetype(page);
			if (!is_migrate_cma(mt) && !is_migrate_isolate(mt))
				mt = migratetype;
		}

		// page: migratetype이 MIGRATE_UNMOVABLE인 page, mt: 0
		set_freepage_migratetype(page, mt);
		// page의 index 필드를 migratetype: 0으로 변경

		// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
		// page->lru: (migratetype이 MIGRATE_UNMOVABLE인 page)->lru
		list = &page->lru;
		// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0] 에 값을 업데이트됨

		// mt: 0, is_migrate_cma(0): false
		if (is_migrate_cma(mt))
			__mod_zone_page_state(zone, NR_FREE_CMA_PAGES,
					      -(1 << order));
	}

	// zone: contig_page_data->node_zones[0], NR_FREE_PAGES: 0, i: 1
	// order: 0, -(1 << 0): -1
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(i << order));
	// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[0], vm_stat[0] 값을 업데이트

	spin_unlock(&zone->lock);
	// spinlock 해제

	// i: 1
	return i;
	// return 1
}

#ifdef CONFIG_NUMA // CONFIG_NUMA=n
/*
 * Called from the vmstat counter updater to drain pagesets of this
 * currently executing processor on remote nodes after they have
 * expired.
 *
 * Note that this function must be called with the thread pinned to
 * a single processor.
 */
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp)
{
	unsigned long flags;
	int to_drain;
	unsigned long batch;

	local_irq_save(flags);
	batch = ACCESS_ONCE(pcp->batch);
	if (pcp->count >= batch)
		to_drain = batch;
	else
		to_drain = pcp->count;
	if (to_drain > 0) {
		free_pcppages_bulk(zone, to_drain, pcp);
		pcp->count -= to_drain;
	}
	local_irq_restore(flags);
}
static bool gfp_thisnode_allocation(gfp_t gfp_mask)
{
	return (gfp_mask & GFP_THISNODE) == GFP_THISNODE;
}
#else
// ARM10C 20140510
// ARM10C 20140517
// gfp_mask: 0x221200
static bool gfp_thisnode_allocation(gfp_t gfp_mask)
{
	return false;
}
#endif

/*
 * Drain pages of the indicated processor.
 *
 * The processor must either be the current processor and the
 * thread pinned to the current processor or a processor that
 * is not online.
 */
static void drain_pages(unsigned int cpu)
{
	unsigned long flags;
	struct zone *zone;

	for_each_populated_zone(zone) {
		struct per_cpu_pageset *pset;
		struct per_cpu_pages *pcp;

		local_irq_save(flags);
		pset = per_cpu_ptr(zone->pageset, cpu);

		pcp = &pset->pcp;
		if (pcp->count) {
			free_pcppages_bulk(zone, pcp->count, pcp);
			pcp->count = 0;
		}
		local_irq_restore(flags);
	}
}

/*
 * Spill all of this CPU's per-cpu pages back into the buddy allocator.
 */
void drain_local_pages(void *arg)
{
	drain_pages(smp_processor_id());
}

/*
 * Spill all the per-cpu pages from all CPUs back into the buddy allocator.
 *
 * Note that this code is protected against sending an IPI to an offline
 * CPU but does not guarantee sending an IPI to newly hotplugged CPUs:
 * on_each_cpu_mask() blocks hotplug and won't talk to offlined CPUs but
 * nothing keeps CPUs from showing up after we populated the cpumask and
 * before the call to on_each_cpu_mask().
 */
void drain_all_pages(void)
{
	int cpu;
	struct per_cpu_pageset *pcp;
	struct zone *zone;

	/*
	 * Allocate in the BSS so we wont require allocation in
	 * direct reclaim path for CONFIG_CPUMASK_OFFSTACK=y
	 */
	static cpumask_t cpus_with_pcps;

	/*
	 * We don't care about racing with CPU hotplug event
	 * as offline notification will cause the notified
	 * cpu to drain that CPU pcps and on_each_cpu_mask
	 * disables preemption as part of its processing
	 */
	for_each_online_cpu(cpu) {
		bool has_pcps = false;
		for_each_populated_zone(zone) {
			pcp = per_cpu_ptr(zone->pageset, cpu);
			if (pcp->pcp.count) {
				has_pcps = true;
				break;
			}
		}
		if (has_pcps)
			cpumask_set_cpu(cpu, &cpus_with_pcps);
		else
			cpumask_clear_cpu(cpu, &cpus_with_pcps);
	}
	on_each_cpu_mask(&cpus_with_pcps, drain_local_pages, NULL, 1);
}

#ifdef CONFIG_HIBERNATION

void mark_free_pages(struct zone *zone)
{
	unsigned long pfn, max_zone_pfn;
	unsigned long flags;
	int order, t;
	struct list_head *curr;

	if (zone_is_empty(zone))
		return;

	spin_lock_irqsave(&zone->lock, flags);

	max_zone_pfn = zone_end_pfn(zone);
	for (pfn = zone->zone_start_pfn; pfn < max_zone_pfn; pfn++)
		if (pfn_valid(pfn)) {
			struct page *page = pfn_to_page(pfn);

			if (!swsusp_page_is_forbidden(page))
				swsusp_unset_page_free(page);
		}

	for_each_migratetype_order(order, t) {
		list_for_each(curr, &zone->free_area[order].free_list[t]) {
			unsigned long i;

			pfn = page_to_pfn(list_entry(curr, struct page, lru));
			for (i = 0; i < (1UL << order); i++)
				swsusp_set_page_free(pfn_to_page(pfn + i));
		}
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif /* CONFIG_PM */

/*
 * Free a 0-order page
 * cold == 1 ? free a cold page : free a hot page
 */
// ARM10C 20140412
// page: 0x20000 (pfn), cold: 0
void free_hot_cold_page(struct page *page, int cold)
{
	// page: 0x20000 (pfn)
	struct zone *zone = page_zone(page);
	// zone: &(&contig_page_data)->node_zones[ZONE_NORMAL]
	struct per_cpu_pages *pcp;
	unsigned long flags;
	int migratetype;

	// page: 0x20000 (pfn)
	if (!free_pages_prepare(page, 0))
		return;
	// page->flags의 NR_PAGEFLAGS 만큼의 하위 비트를 전부 지워줌

	// page: 0x20000 (pfn), get_pageblock_migratetype(page): 0x2
	migratetype = get_pageblock_migratetype(page);
	// migratetype: 0x2

	// page: 0x20000 (pfn), migratetype: 0x2
	set_freepage_migratetype(page, migratetype);
	// page->index에 migratetype을 설정

	local_irq_save(flags);
	// flags에 cpsr 값 저장

	// PGFREE: 7
	__count_vm_event(PGFREE);
	// *(&(vm_event_states.event[PGFREE]) + __my_cpu_offset): 1

	/*
	 * We only track unmovable, reclaimable and movable on pcp lists.
	 * Free ISOLATE pages back to the allocator because they are being
	 * offlined but treat RESERVE as movable pages so we can get those
	 * areas back if necessary. Otherwise, we may have to free
	 * excessively into the page allocator
	 */
	// migratetype: 0x2, MIGRATE_PCPTYPES: 3
	if (migratetype >= MIGRATE_PCPTYPES) {
		if (unlikely(is_migrate_isolate(migratetype))) {
			free_one_page(zone, page, 0, migratetype);
			goto out;
		}
		migratetype = MIGRATE_MOVABLE;
	}

	// zone->pageset: &contig_page_data->node_zones[ZONE_NORMAL].pageset: &boot_pageset
	// this_cpu_ptr(zone->pageset): (&boot_pageset) + (__per_cpu_offset[0]);
	pcp = &this_cpu_ptr(zone->pageset)->pcp;
	// pcp: &((&boot_pageset) + (__per_cpu_offset[0]))->pcp

	// cold 와 hot의 의미?
	// cold 변수가 1 이면 (cold)  리스트의 마지막에 붙여 천천히 리스트에서 검색되도록 하며
	// 0 이면 (hot) 리스트의 처음에 붙여 빨리 검색되어 사용되도록 한다.
	// cold: 0
	if (cold)
		list_add_tail(&page->lru, &pcp->lists[migratetype]);
	else
		// migratetype: 0x2, &pcp->lists[2]: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists[2]
		list_add(&page->lru, &pcp->lists[migratetype]);
		// &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.lists[2]에 page->lru 연결

	// pcp->count: (&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.count: 0
	pcp->count++;
	// pcp->count: (&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.count: 1

	// pcp->high: (&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.high: 0
	if (pcp->count >= pcp->high) {
		// pcp->batch: (&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.batch: 1
		unsigned long batch = ACCESS_ONCE(pcp->batch);
		// batch: 1

		// zone: &(&contig_page_data)->node_zones[ZONE_NORMAL], batch: 1,
		// pcp: &(&((&boot_pageset) + (__per_cpu_offset[0])))->pcp
		free_pcppages_bulk(zone, batch, pcp);

		// pcp->count: (&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.count: 1, batch: 1
		pcp->count -= batch;
		// pcp->count: (&((&boot_pageset) + (__per_cpu_offset[0])))->pcp.count: 0
	}

out:
	local_irq_restore(flags);
	// flags에 저장된 cpsr 값 restore
}

/*
 * Free a list of 0-order pages
 */
void free_hot_cold_page_list(struct list_head *list, int cold)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, list, lru) {
		trace_mm_page_free_batched(page, cold);
		free_hot_cold_page(page, cold);
	}
}

/*
 * split_page takes a non-compound higher-order page, and splits it into
 * n (1<<order) sub-pages: page[0..n]
 * Each sub-page must be freed individually.
 *
 * Note: this is probably too low level an operation for use in drivers.
 * Please consult with lkml before using this in your driver.
 */
void split_page(struct page *page, unsigned int order)
{
	int i;

	VM_BUG_ON(PageCompound(page));
	VM_BUG_ON(!page_count(page));

#ifdef CONFIG_KMEMCHECK
	/*
	 * Split shadow pages too, because free(page[0]) would
	 * otherwise free the whole shadow.
	 */
	if (kmemcheck_page_is_tracked(page))
		split_page(virt_to_page(page[0].shadow), order);
#endif

	for (i = 1; i < (1 << order); i++)
		set_page_refcounted(page + i);
}
EXPORT_SYMBOL_GPL(split_page);

static int __isolate_free_page(struct page *page, unsigned int order)
{
	unsigned long watermark;
	struct zone *zone;
	int mt;

	BUG_ON(!PageBuddy(page));

	zone = page_zone(page);
	mt = get_pageblock_migratetype(page);

	if (!is_migrate_isolate(mt)) {
		/* Obey watermarks as if the page was being allocated */
		watermark = low_wmark_pages(zone) + (1 << order);
		if (!zone_watermark_ok(zone, 0, watermark, 0, 0))
			return 0;

		__mod_zone_freepage_state(zone, -(1UL << order), mt);
	}

	/* Remove page from free list */
	list_del(&page->lru);
	zone->free_area[order].nr_free--;
	rmv_page_order(page);

	/* Set the pageblock if the isolated page is at least a pageblock */
	if (order >= pageblock_order - 1) {
		struct page *endpage = page + (1 << order) - 1;
		for (; page < endpage; page += pageblock_nr_pages) {
			int mt = get_pageblock_migratetype(page);
			if (!is_migrate_isolate(mt) && !is_migrate_cma(mt))
				set_pageblock_migratetype(page,
							  MIGRATE_MOVABLE);
		}
	}

	return 1UL << order;
}

/*
 * Similar to split_page except the page is already free. As this is only
 * being used for migration, the migratetype of the block also changes.
 * As this is called with interrupts disabled, the caller is responsible
 * for calling arch_alloc_page() and kernel_map_page() after interrupts
 * are enabled.
 *
 * Note: this is probably too low level an operation for use in drivers.
 * Please consult with lkml before using this in your driver.
 */
int split_free_page(struct page *page)
{
	unsigned int order;
	int nr_pages;

	order = page_order(page);

	nr_pages = __isolate_free_page(page, order);
	if (!nr_pages)
		return 0;

	/* Split into individual pages */
	set_page_refcounted(page);
	split_page(page, order);
	return nr_pages;
}

/*
 * Really, prep_compound_page() should be called from __rmqueue_bulk().  But
 * we cheat by calling it from here, in the order > 0 path.  Saves a branch
 * or two.
 */
// ARM10C 20140510
// preferred_zone: (&contig_page_data)->node_zones[0]
// zone: contig_page_data->node_zones[0], order: 0, gfp_mask: 0x221200
// migratetype: MIGRATE_UNMOVABLE: 0
static inline
struct page *buffered_rmqueue(struct zone *preferred_zone,
			struct zone *zone, int order, gfp_t gfp_flags,
			int migratetype)
{
	unsigned long flags;
	struct page *page;
	// gfp_flags: 0x221200, __GFP_COLD: 0x100u
	int cold = !!(gfp_flags & __GFP_COLD);
	// cold: 0

again:
	// order: 0
	if (likely(order == 0)) {
		struct per_cpu_pages *pcp;
		struct list_head *list;

		local_irq_save(flags);
		// zone->pageset: contig_page_data->node_zones[0].pageset: &boot_pageset
		// this_cpu_ptr(&boot_pageset): &boot_pageset + (__per_cpu_offset[0])
		// this_cpu_ptr(&boot_pageset)->pcp: (&boot_pageset + (__per_cpu_offset[0]))->pcp
		pcp = &this_cpu_ptr(zone->pageset)->pcp;
		// pcp: (&boot_pageset + (__per_cpu_offset[0]))->pcp

		// migratetype: 0, pcp->lists[0]: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
		list = &pcp->lists[migratetype];
		// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]

// 2014/05/10 종료
// 2014/05/17 시작

		// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
		// list_empty((&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]): 1
		if (list_empty(list)) {
			// pcp->count: (&boot_pageset + (__per_cpu_offset[0]))->pcp.count: 0
			// zone: contig_page_data->node_zones[0],
			// pcp->batch: (&boot_pageset + (__per_cpu_offset[0]))->pcp.batch: 1
			// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
			// migratetype: MIGRATE_UNMOVABLE: 0, cold: 0
			// rmqueue_bulk(contig_page_data->node_zones[0], 0, 1,
			//		(&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0] , 0, 0): 1
			pcp->count += rmqueue_bulk(zone, 0,
					pcp->batch, list,
					migratetype, cold);
			// pcp->count: (&boot_pageset + (__per_cpu_offset[0]))->pcp.count: 1

			// list: (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]
			// list_empty((&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0]): 0
			if (unlikely(list_empty(list)))
				goto failed;
		}

		// cold: 0
		if (cold)
			page = list_entry(list->prev, struct page, lru);
		else
			// list->next: ((&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0])->next
			page = list_entry(list->next, struct page, lru);
			// page: migratetype이 MIGRATE_UNMOVABLE인 page

		// page->lru: (migratetype이 MIGRATE_UNMOVABLE인 page)->lru
		list_del(&page->lru);
		// (&boot_pageset + (__per_cpu_offset[0]))->pcp.lists[0] 에서 현재 page->lru 삭제

		// pcp->count: (&boot_pageset + (__per_cpu_offset[0]))->pcp.count: 1
		pcp->count--;
		// pcp->count: (&boot_pageset + (__per_cpu_offset[0]))->pcp.count: 0
	} else {
		if (unlikely(gfp_flags & __GFP_NOFAIL)) {
			/*
			 * __GFP_NOFAIL is not to be used in new code.
			 *
			 * All __GFP_NOFAIL callers should be fixed so that they
			 * properly detect and handle allocation failures.
			 *
			 * We most definitely don't want callers attempting to
			 * allocate greater than order-1 page units with
			 * __GFP_NOFAIL.
			 */
			WARN_ON_ONCE(order > 1);
		}
		spin_lock_irqsave(&zone->lock, flags);
		page = __rmqueue(zone, order, migratetype);
		spin_unlock(&zone->lock);
		if (!page)
			goto failed;
		__mod_zone_freepage_state(zone, -(1 << order),
					  get_pageblock_migratetype(page));
	}

// 2014/05/17 종료
// 2014/05/24 시작

	/*
	 * NOTE: GFP_THISNODE allocations do not partake in the kswapd
	 * aging protocol, so they can't be fair.
	 */
	// gfp_flags: 0x221200
	// gfp_thisnode_allocation(0x221200): false
	if (!gfp_thisnode_allocation(gfp_flags))
		// zone: contig_page_data->node_zones[0], NR_ALLOC_BATCH: 1, order: 0
		__mod_zone_page_state(zone, NR_ALLOC_BATCH, -(1 << order));
		// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[1]: 0x2efd5, vm_stat[1]: 0x2efd5
		// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[1], vm_stat[1] 값을 업데이트

	// zone: contig_page_data->node_zones[0], order: 0
	__count_zone_vm_events(PGALLOC, zone, 1 << order);
	// vm_event_states.event[PGALLOC_NORMAL]: 1 업데이트

	// preferred_zone: (&contig_page_data)->node_zones[0],
	// zone: contig_page_data->node_zones[0], gfp_flags: 0x221200
	zone_statistics(preferred_zone, zone, gfp_flags); // null function
	local_irq_restore(flags);

	// zone: contig_page_data->node_zones[0],
	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	// bad_range(contig_page_data->node_zones[0],
	//	    migratetype이 MIGRATE_UNMOVABLE인 page): 0
	VM_BUG_ON(bad_range(zone, page));

	// page: migratetype이 MIGRATE_UNMOVABLE인 page, order: 0, gfp_flags: 0x221200
	// prep_new_page (migratetype이 MIGRATE_UNMOVABLE인 page, 0, 0x221200): 0
	if (prep_new_page(page, order, gfp_flags))
		goto again;

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	return page;

failed:
	local_irq_restore(flags);
	return NULL;
}

#ifdef CONFIG_FAIL_PAGE_ALLOC // CONFIG_FAIL_PAGE_ALLOC=n

static struct {
	struct fault_attr attr;

	u32 ignore_gfp_highmem;
	u32 ignore_gfp_wait;
	u32 min_order;
} fail_page_alloc = {
	.attr = FAULT_ATTR_INITIALIZER,
	.ignore_gfp_wait = 1,
	.ignore_gfp_highmem = 1,
	.min_order = 1,
};

static int __init setup_fail_page_alloc(char *str)
{
	return setup_fault_attr(&fail_page_alloc.attr, str);
}
__setup("fail_page_alloc=", setup_fail_page_alloc);

static bool should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	if (order < fail_page_alloc.min_order)
		return false;
	if (gfp_mask & __GFP_NOFAIL)
		return false;
	if (fail_page_alloc.ignore_gfp_highmem && (gfp_mask & __GFP_HIGHMEM))
		return false;
	if (fail_page_alloc.ignore_gfp_wait && (gfp_mask & __GFP_WAIT))
		return false;

	return should_fail(&fail_page_alloc.attr, 1 << order);
}

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS

static int __init fail_page_alloc_debugfs(void)
{
	umode_t mode = S_IFREG | S_IRUSR | S_IWUSR;
	struct dentry *dir;

	dir = fault_create_debugfs_attr("fail_page_alloc", NULL,
					&fail_page_alloc.attr);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	if (!debugfs_create_bool("ignore-gfp-wait", mode, dir,
				&fail_page_alloc.ignore_gfp_wait))
		goto fail;
	if (!debugfs_create_bool("ignore-gfp-highmem", mode, dir,
				&fail_page_alloc.ignore_gfp_highmem))
		goto fail;
	if (!debugfs_create_u32("min-order", mode, dir,
				&fail_page_alloc.min_order))
		goto fail;

	return 0;
fail:
	debugfs_remove_recursive(dir);

	return -ENOMEM;
}

late_initcall(fail_page_alloc_debugfs);

#endif /* CONFIG_FAULT_INJECTION_DEBUG_FS */

#else /* CONFIG_FAIL_PAGE_ALLOC */

// ARM10C 20140426
static inline bool should_fail_alloc_page(gfp_t gfp_mask, unsigned int order)
{
	return false;
}

#endif /* CONFIG_FAIL_PAGE_ALLOC */

/*
 * Return true if free pages are above 'mark'. This takes into account the order
 * of the allocation.
 */
// ARM10C 20140510
// z: contig_page_data->node_zones[0], order: 0, mark: 0
// classzone_idx: 0 alloc_flags: 0x41, ????
static bool __zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags, long free_pages)
{
	/* free_pages my go negative - that's OK */
	// mark: 0
	long min = mark;
	// min: 0
	// classzone_idx: 0, z->lowmem_reserve[0]: contig_page_data->node_zones[0].lowmem_reserve[0]: 0
	long lowmem_reserve = z->lowmem_reserve[classzone_idx];
	// lowmem_reserve: 0
	int o;
	long free_cma = 0;

	// free_pages: ????
	free_pages -= (1 << order) - 1;
	// free_pages: ????

	// alloc_flags: 0x41, ALLOC_HIGH: 0x20
	if (alloc_flags & ALLOC_HIGH)
		min -= min / 2;

	// alloc_flags: 0x41, ALLOC_HARDER: 0x10
	if (alloc_flags & ALLOC_HARDER)
		min -= min / 4;

#ifdef CONFIG_CMA // CONFIG_CMA=n
	/* If allocation can't use CMA areas don't use free CMA pages */
	if (!(alloc_flags & ALLOC_CMA))
		free_cma = zone_page_state(z, NR_FREE_CMA_PAGES);
#endif

	// free_pages: ????, free_cma: 0, min: 0, lowmem_reserve: 0
	if (free_pages - free_cma <= min + lowmem_reserve)
		return false;

	// order: 0
	for (o = 0; o < order; o++) {
		/* At the next order, this order's pages become unavailable */
		free_pages -= z->free_area[o].nr_free << o;

		/* Require fewer higher order pages to be free */
		min >>= 1;

		if (free_pages <= min)
			return false;
	}
	return true;
	// return true
}

// ARM10C 20140510
// zone: contig_page_data->node_zones[0], order: 0, mark: 0
// classzone_idx: 0 alloc_flags: 0x41
bool zone_watermark_ok(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags)
{
	// z: contig_page_data->node_zones[0], order: 0, mark: 0
	// classzone_idx: 0 alloc_flags: 0x41
	// zone_page_state(contig_page_data->node_zones[0], NR_FREE_PAGES): ????
	return __zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
					zone_page_state(z, NR_FREE_PAGES));
	// return 1
}

bool zone_watermark_ok_safe(struct zone *z, int order, unsigned long mark,
		      int classzone_idx, int alloc_flags)
{
	long free_pages = zone_page_state(z, NR_FREE_PAGES);

	if (z->percpu_drift_mark && free_pages < z->percpu_drift_mark)
		free_pages = zone_page_state_snapshot(z, NR_FREE_PAGES);

	return __zone_watermark_ok(z, order, mark, classzone_idx, alloc_flags,
								free_pages);
}

#ifdef CONFIG_NUMA	// ARM10C CONFIG_NUMA = n 
/*
 * zlc_setup - Setup for "zonelist cache".  Uses cached zone data to
 * skip over zones that are not allowed by the cpuset, or that have
 * been recently (in last second) found to be nearly full.  See further
 * comments in mmzone.h.  Reduces cache footprint of zonelist scans
 * that have to skip over a lot of full or unallowed zones.
 *
 * If the zonelist cache is present in the passed zonelist, then
 * returns a pointer to the allowed node mask (either the current
 * tasks mems_allowed, or node_states[N_MEMORY].)
 *
 * If the zonelist cache is not available for this zonelist, does
 * nothing and returns NULL.
 *
 * If the fullzones BITMAP in the zonelist cache is stale (more than
 * a second since last zap'd) then we zap it out (clear its bits.)
 *
 * We hold off even calling zlc_setup, until after we've checked the
 * first zone in the zonelist, on the theory that most allocations will
 * be satisfied from that first zone, so best to examine that zone as
 * quickly as we can.
 */
static nodemask_t *zlc_setup(struct zonelist *zonelist, int alloc_flags)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */
	nodemask_t *allowednodes;	/* zonelist_cache approximation */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return NULL;

	if (time_after(jiffies, zlc->last_full_zap + HZ)) {
		bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
		zlc->last_full_zap = jiffies;
	}

	allowednodes = !in_interrupt() && (alloc_flags & ALLOC_CPUSET) ?
					&cpuset_current_mems_allowed :
					&node_states[N_MEMORY];
	return allowednodes;
}

/*
 * Given 'z' scanning a zonelist, run a couple of quick checks to see
 * if it is worth looking at further for free memory:
 *  1) Check that the zone isn't thought to be full (doesn't have its
 *     bit set in the zonelist_cache fullzones BITMAP).
 *  2) Check that the zones node (obtained from the zonelist_cache
 *     z_to_n[] mapping) is allowed in the passed in allowednodes mask.
 * Return true (non-zero) if zone is worth looking at further, or
 * else return false (zero) if it is not.
 *
 * This check -ignores- the distinction between various watermarks,
 * such as GFP_HIGH, GFP_ATOMIC, PF_MEMALLOC, ...  If a zone is
 * found to be full for any variation of these watermarks, it will
 * be considered full for up to one second by all requests, unless
 * we are so low on memory on all allowed nodes that we are forced
 * into the second scan of the zonelist.
 *
 * In the second scan we ignore this zonelist cache and exactly
 * apply the watermarks to all zones, even it is slower to do so.
 * We are low on memory in the second scan, and should leave no stone
 * unturned looking for a free page.
 */
static int zlc_zone_worth_trying(struct zonelist *zonelist, struct zoneref *z,
						nodemask_t *allowednodes)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */
	int i;				/* index of *z in zonelist zones */
	int n;				/* node that zone *z is on */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return 1;

	i = z - zonelist->_zonerefs;
	n = zlc->z_to_n[i];

	/* This zone is worth trying if it is allowed but not full */
	return node_isset(n, *allowednodes) && !test_bit(i, zlc->fullzones);
}

/*
 * Given 'z' scanning a zonelist, set the corresponding bit in
 * zlc->fullzones, so that subsequent attempts to allocate a page
 * from that zone don't waste time re-examining it.
 */
static void zlc_mark_zone_full(struct zonelist *zonelist, struct zoneref *z)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */
	int i;				/* index of *z in zonelist zones */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return;

	i = z - zonelist->_zonerefs;

	set_bit(i, zlc->fullzones);
}

/*
 * clear all zones full, called after direct reclaim makes progress so that
 * a zone that was recently full is not skipped over for up to a second
 */
static void zlc_clear_zones_full(struct zonelist *zonelist)
{
	struct zonelist_cache *zlc;	/* cached zonelist speedup info */

	zlc = zonelist->zlcache_ptr;
	if (!zlc)
		return;

	bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
}

static bool zone_local(struct zone *local_zone, struct zone *zone)
{
	return local_zone->node == zone->node;
}

static bool zone_allows_reclaim(struct zone *local_zone, struct zone *zone)
{
	return node_isset(local_zone->node, zone->zone_pgdat->reclaim_nodes);
}

static void __paginginit init_zone_allows_reclaim(int nid)
{
	int i;

	for_each_online_node(i)
		if (node_distance(nid, i) <= RECLAIM_DISTANCE)
			node_set(i, NODE_DATA(nid)->reclaim_nodes);
		else
			zone_reclaim_mode = 1;
}

#else	/* CONFIG_NUMA */

static nodemask_t *zlc_setup(struct zonelist *zonelist, int alloc_flags)
{
	return NULL;
}

// ARM10C 20140510
// zonelist: contig_page_data->node_zonelists, allowednodes: NULL
static int zlc_zone_worth_trying(struct zonelist *zonelist, struct zoneref *z,
				nodemask_t *allowednodes)
{
	return 1;
}

static void zlc_mark_zone_full(struct zonelist *zonelist, struct zoneref *z)
{
}

static void zlc_clear_zones_full(struct zonelist *zonelist)
{
}

// ARM10C 20140510
// preferred_zone: (&contig_page_data)->node_zones[0]
// zone: contig_page_data->node_zones[0]
static bool zone_local(struct zone *local_zone, struct zone *zone)
{
	return true;
}

static bool zone_allows_reclaim(struct zone *local_zone, struct zone *zone)
{
	return true;
}

// ARM10C 20140111 
static inline void init_zone_allows_reclaim(int nid)
{
}
#endif	/* CONFIG_NUMA */

/*
 * get_page_from_freelist goes through the zonelist trying to allocate
 * a page.
 */
// ARM10C 20140426
// ARM10C 20140510
// 0x221200, nodemask: NULL, order: 0
// zonelist: contig_page_data->node_zonelists, high_zoneidx: ZONE_NORMAL: 0
// alloc_flags: 0x41, preferred_zone: (&contig_page_data)->node_zones[0]
// migratetype: MIGRATE_UNMOVABLE: 0
static struct page *
get_page_from_freelist(gfp_t gfp_mask, nodemask_t *nodemask, unsigned int order,
		struct zonelist *zonelist, int high_zoneidx, int alloc_flags,
		struct zone *preferred_zone, int migratetype)
{
	struct zoneref *z;
	struct page *page = NULL;
	int classzone_idx;
	struct zone *zone;
	nodemask_t *allowednodes = NULL;/* zonelist_cache approximation */
	int zlc_active = 0;		/* set if using zonelist_cache */
	int did_zlc_setup = 0;		/* just call zlc_setup() one time */

	// preferred_zone: (&contig_page_data)->node_zones[0]
	// zone_idx((&contig_page_data)->node_zones[0]): 0
	classzone_idx = zone_idx(preferred_zone);
	// classzone_idx: 0

zonelist_scan:
	/*
	 * Scan zonelist, looking for a zone with enough free.
	 * See also __cpuset_node_allowed_softwall() comment in kernel/cpuset.c.
	 */
	// zonelist: contig_page_data->node_zonelists, high_zoneidx: ZONE_NORMAL: 0
	// nodemask: NULL
	for_each_zone_zonelist_nodemask(zone, z, zonelist,
						high_zoneidx, nodemask) {
	// for (z = first_zones_zonelist(contig_page_data->node_zonelists, ZONE_NORMAL, NULL, &zone);
	//	zone; z = next_zones_zonelist(++z, ZONE_NORMAL, NULL, &zone))

		unsigned long mark;

		// IS_ENABLED(CONFIG_NUMA): 0, zlc_active: 0
		// zonelist: contig_page_data->node_zonelists, allowednodes: NULL
		// zlc_zone_worth_trying(contig_page_data->node_zonelists, z, NULL): 1
		if (IS_ENABLED(CONFIG_NUMA) && zlc_active &&
			!zlc_zone_worth_trying(zonelist, z, allowednodes))
				continue;

		// alloc_flags: 0x41, ALLOC_CPUSET: 0x40
		// zone: contig_page_data->node_zones[0], gfp_mask: 0x221200
		// cpuset_zone_allowed_softwall(contig_page_data->node_zones[0], 0x221200): 1
		if ((alloc_flags & ALLOC_CPUSET) &&
			!cpuset_zone_allowed_softwall(zone, gfp_mask))
				continue;

		// ALLOC_NO_WATERMARKS: 0x04, NR_WMARK: 3
		BUILD_BUG_ON(ALLOC_NO_WATERMARKS < NR_WMARK);

		// alloc_flags: 0x41, ALLOC_NO_WATERMARKS: 0x04
		if (unlikely(alloc_flags & ALLOC_NO_WATERMARKS))
			goto try_this_zone;
		/*
		 * Distribute pages in proportion to the individual
		 * zone size to ensure fair page aging.  The zone a
		 * page was allocated in should have no effect on the
		 * time the page has in memory before being reclaimed.
		 *
		 * Try to stay in local zones in the fastpath.  If
		 * that fails, the slowpath is entered, which will do
		 * another pass starting with the local zones, but
		 * ultimately fall back to remote zones that do not
		 * partake in the fairness round-robin cycle of this
		 * zonelist.
		 *
		 * NOTE: GFP_THISNODE allocations do not partake in
		 * the kswapd aging protocol, so they can't be fair.
		 */
		// alloc_flags: 0x41, ALLOC_WMARK_LOW: 1, gfp_mask: 0x221200
		// gfp_thisnode_allocation(0x221200): false
		if ((alloc_flags & ALLOC_WMARK_LOW) &&
		    !gfp_thisnode_allocation(gfp_mask)) {
			// zone: contig_page_data->node_zones[0], NR_ALLOC_BATCH: 1
			// zone_page_state(contig_page_data->node_zones[0], NR_ALLOC_BATCH): 0x2efd6
			if (zone_page_state(zone, NR_ALLOC_BATCH) <= 0)
				continue;

			// preferred_zone: (&contig_page_data)->node_zones[0]
			// zone: contig_page_data->node_zones[0]
			// zone_local((&contig_page_data)->node_zones[0], contig_page_data->node_zones[0]): true
			if (!zone_local(preferred_zone, zone))
				continue;
		}
		/*
		 * When allocating a page cache page for writing, we
		 * want to get it from a zone that is within its dirty
		 * limit, such that no single zone holds more than its
		 * proportional share of globally allowed dirty pages.
		 * The dirty limits take into account the zone's
		 * lowmem reserves and high watermark so that kswapd
		 * should be able to balance it without having to
		 * write pages from its LRU list.
		 *
		 * This may look like it could increase pressure on
		 * lower zones by failing allocations in higher zones
		 * before they are full.  But the pages that do spill
		 * over are limited as the lower zones are protected
		 * by this very same mechanism.  It should not become
		 * a practical burden to them.
		 *
		 * XXX: For now, allow allocations to potentially
		 * exceed the per-zone dirty limit in the slowpath
		 * (ALLOC_WMARK_LOW unset) before going into reclaim,
		 * which is important when on a NUMA setup the allowed
		 * zones are together not big enough to reach the
		 * global limit.  The proper fix for these situations
		 * will require awareness of zones in the
		 * dirty-throttling and the flusher threads.
		 */
		// alloc_flags: 0x41, ALLOC_WMARK_LOW: 1, gfp_mask: 0x221200
		// __GFP_WRITE: 0x1000000u, zone: contig_page_data->node_zones[0]
		// zone_dirty_ok(contig_page_data->node_zones[0]): 1
		if ((alloc_flags & ALLOC_WMARK_LOW) &&
		    (gfp_mask & __GFP_WRITE) && !zone_dirty_ok(zone))
			goto this_zone_full;

		// alloc_flags: 0x41, ALLOC_WMARK_MASK: 0x03
		// zone->watermark[0x1]: contig_page_data->node_zones[0].watermark[1]: 0
		mark = zone->watermark[alloc_flags & ALLOC_WMARK_MASK];
		// mark: 0

		// zone: contig_page_data->node_zones[0], order: 0, mark: 0
		// classzone_idx: 0 alloc_flags: 0x41
		// zone_watermark_ok(contig_page_data->node_zones[0], 0, 0, 0, 0x41): 1
		if (!zone_watermark_ok(zone, order, mark,
				       classzone_idx, alloc_flags)) {
			int ret;

			if (IS_ENABLED(CONFIG_NUMA) &&
					!did_zlc_setup && nr_online_nodes > 1) {
				/*
				 * we do zlc_setup if there are multiple nodes
				 * and before considering the first zone allowed
				 * by the cpuset.
				 */
				allowednodes = zlc_setup(zonelist, alloc_flags);
				zlc_active = 1;
				did_zlc_setup = 1;
			}

			if (zone_reclaim_mode == 0 ||
			    !zone_allows_reclaim(preferred_zone, zone))
				goto this_zone_full;

			/*
			 * As we may have just activated ZLC, check if the first
			 * eligible zone has failed zone_reclaim recently.
			 */
			if (IS_ENABLED(CONFIG_NUMA) && zlc_active &&
				!zlc_zone_worth_trying(zonelist, z, allowednodes))
				continue;

			ret = zone_reclaim(zone, gfp_mask, order);
			switch (ret) {
			case ZONE_RECLAIM_NOSCAN:
				/* did not scan */
				continue;
			case ZONE_RECLAIM_FULL:
				/* scanned but unreclaimable */
				continue;
			default:
				/* did we reclaim enough */
				if (zone_watermark_ok(zone, order, mark,
						classzone_idx, alloc_flags))
					goto try_this_zone;

				/*
				 * Failed to reclaim enough to meet watermark.
				 * Only mark the zone full if checking the min
				 * watermark or if we failed to reclaim just
				 * 1<<order pages or else the page allocator
				 * fastpath will prematurely mark zones full
				 * when the watermark is between the low and
				 * min watermarks.
				 */
				if (((alloc_flags & ALLOC_WMARK_MASK) == ALLOC_WMARK_MIN) ||
				    ret == ZONE_RECLAIM_SOME)
					goto this_zone_full;

				continue;
			}
		}

try_this_zone:
		// preferred_zone: (&contig_page_data)->node_zones[0]
		// zone: contig_page_data->node_zones[0], order: 0, gfp_mask: 0x221200
		// migratetype: MIGRATE_UNMOVABLE: 0
		page = buffered_rmqueue(preferred_zone, zone, order,
						gfp_mask, migratetype);
		// page: migratetype이 MIGRATE_UNMOVABLE인 page

		if (page)
			break;
			// break 로 loop 탈출
this_zone_full:
		if (IS_ENABLED(CONFIG_NUMA))
			zlc_mark_zone_full(zonelist, z);
	}

	// IS_ENABLED(CONFIG_NUMA): 0, page: migratetype이 MIGRATE_UNMOVABLE인 page, zlc_active: 0
	if (unlikely(IS_ENABLED(CONFIG_NUMA) && page == NULL && zlc_active)) {
		/* Disable zlc cache for second zonelist scan */
		zlc_active = 0;
		goto zonelist_scan;
	}

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	if (page)
		/*
		 * page->pfmemalloc is set when ALLOC_NO_WATERMARKS was
		 * necessary to allocate the page. The expectation is
		 * that the caller is taking steps that will free more
		 * memory. The caller should avoid the page being used
		 * for !PFMEMALLOC purposes.
		 */
		// alloc_flags: 0x41, ALLOC_NO_WATERMARKS: 0x04
		page->pfmemalloc = !!(alloc_flags & ALLOC_NO_WATERMARKS);
		// page->pfmemalloc: 0

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	return page;
}

/*
 * Large machines with many possible nodes should not always dump per-node
 * meminfo in irq context.
 */
static inline bool should_suppress_show_mem(void)
{
	bool ret = false;

#if NODES_SHIFT > 8
	ret = in_interrupt();
#endif
	return ret;
}

static DEFINE_RATELIMIT_STATE(nopage_rs,
		DEFAULT_RATELIMIT_INTERVAL,
		DEFAULT_RATELIMIT_BURST);

void warn_alloc_failed(gfp_t gfp_mask, int order, const char *fmt, ...)
{
	unsigned int filter = SHOW_MEM_FILTER_NODES;

	if ((gfp_mask & __GFP_NOWARN) || !__ratelimit(&nopage_rs) ||
	    debug_guardpage_minorder() > 0)
		return;

	/*
	 * Walking all memory to count page types is very expensive and should
	 * be inhibited in non-blockable contexts.
	 */
	if (!(gfp_mask & __GFP_WAIT))
		filter |= SHOW_MEM_FILTER_PAGE_COUNT;

	/*
	 * This documents exceptions given to allocations in certain
	 * contexts that are allowed to allocate outside current's set
	 * of allowed nodes.
	 */
	if (!(gfp_mask & __GFP_NOMEMALLOC))
		if (test_thread_flag(TIF_MEMDIE) ||
		    (current->flags & (PF_MEMALLOC | PF_EXITING)))
			filter &= ~SHOW_MEM_FILTER_NODES;
	if (in_interrupt() || !(gfp_mask & __GFP_WAIT))
		filter &= ~SHOW_MEM_FILTER_NODES;

	if (fmt) {
		struct va_format vaf;
		va_list args;

		va_start(args, fmt);

		vaf.fmt = fmt;
		vaf.va = &args;

		pr_warn("%pV", &vaf);

		va_end(args);
	}

	pr_warn("%s: page allocation failure: order:%d, mode:0x%x\n",
		current->comm, order, gfp_mask);

	dump_stack();
	if (!should_suppress_show_mem())
		show_mem(filter);
}

static inline int
should_alloc_retry(gfp_t gfp_mask, unsigned int order,
				unsigned long did_some_progress,
				unsigned long pages_reclaimed)
{
	/* Do not loop if specifically requested */
	if (gfp_mask & __GFP_NORETRY)
		return 0;

	/* Always retry if specifically requested */
	if (gfp_mask & __GFP_NOFAIL)
		return 1;

	/*
	 * Suspend converts GFP_KERNEL to __GFP_WAIT which can prevent reclaim
	 * making forward progress without invoking OOM. Suspend also disables
	 * storage devices so kswapd will not help. Bail if we are suspending.
	 */
	if (!did_some_progress && pm_suspended_storage())
		return 0;

	/*
	 * In this implementation, order <= PAGE_ALLOC_COSTLY_ORDER
	 * means __GFP_NOFAIL, but that may not be true in other
	 * implementations.
	 */
	if (order <= PAGE_ALLOC_COSTLY_ORDER)
		return 1;

	/*
	 * For order > PAGE_ALLOC_COSTLY_ORDER, if __GFP_REPEAT is
	 * specified, then we retry until we no longer reclaim any pages
	 * (above), or we've reclaimed an order of pages at least as
	 * large as the allocation's order. In both cases, if the
	 * allocation still fails, we stop retrying.
	 */
	if (gfp_mask & __GFP_REPEAT && pages_reclaimed < (1 << order))
		return 1;

	return 0;
}

static inline struct page *
__alloc_pages_may_oom(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	struct page *page;

	/* Acquire the OOM killer lock for the zones in zonelist */
	if (!try_set_zonelist_oom(zonelist, gfp_mask)) {
		schedule_timeout_uninterruptible(1);
		return NULL;
	}

	/*
	 * Go through the zonelist yet one more time, keep very high watermark
	 * here, this is only to catch a parallel oom killing, we must fail if
	 * we're still under heavy pressure.
	 */
	page = get_page_from_freelist(gfp_mask|__GFP_HARDWALL, nodemask,
		order, zonelist, high_zoneidx,
		ALLOC_WMARK_HIGH|ALLOC_CPUSET,
		preferred_zone, migratetype);
	if (page)
		goto out;

	if (!(gfp_mask & __GFP_NOFAIL)) {
		/* The OOM killer will not help higher order allocs */
		if (order > PAGE_ALLOC_COSTLY_ORDER)
			goto out;
		/* The OOM killer does not needlessly kill tasks for lowmem */
		if (high_zoneidx < ZONE_NORMAL)
			goto out;
		/*
		 * GFP_THISNODE contains __GFP_NORETRY and we never hit this.
		 * Sanity check for bare calls of __GFP_THISNODE, not real OOM.
		 * The caller should handle page allocation failure by itself if
		 * it specifies __GFP_THISNODE.
		 * Note: Hugepage uses it but will hit PAGE_ALLOC_COSTLY_ORDER.
		 */
		if (gfp_mask & __GFP_THISNODE)
			goto out;
	}
	/* Exhausted what can be done so it's blamo time */
	out_of_memory(zonelist, gfp_mask, order, nodemask, false);

out:
	clear_zonelist_oom(zonelist, gfp_mask);
	return page;
}

#ifdef CONFIG_COMPACTION
/* Try memory compaction for high-order allocations before reclaim */
static struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, bool sync_migration,
	bool *contended_compaction, bool *deferred_compaction,
	unsigned long *did_some_progress)
{
	if (!order)
		return NULL;

	if (compaction_deferred(preferred_zone, order)) {
		*deferred_compaction = true;
		return NULL;
	}

	current->flags |= PF_MEMALLOC;
	*did_some_progress = try_to_compact_pages(zonelist, order, gfp_mask,
						nodemask, sync_migration,
						contended_compaction);
	current->flags &= ~PF_MEMALLOC;

	if (*did_some_progress != COMPACT_SKIPPED) {
		struct page *page;

		/* Page migration frees to the PCP lists but we want merging */
		drain_pages(get_cpu());
		put_cpu();

		page = get_page_from_freelist(gfp_mask, nodemask,
				order, zonelist, high_zoneidx,
				alloc_flags & ~ALLOC_NO_WATERMARKS,
				preferred_zone, migratetype);
		if (page) {
			preferred_zone->compact_blockskip_flush = false;
			preferred_zone->compact_considered = 0;
			preferred_zone->compact_defer_shift = 0;
			if (order >= preferred_zone->compact_order_failed)
				preferred_zone->compact_order_failed = order + 1;
			count_vm_event(COMPACTSUCCESS);
			return page;
		}

		/*
		 * It's bad if compaction run occurs and fails.
		 * The most likely reason is that pages exist,
		 * but not enough to satisfy watermarks.
		 */
		count_vm_event(COMPACTFAIL);

		/*
		 * As async compaction considers a subset of pageblocks, only
		 * defer if the failure was a sync compaction failure.
		 */
		if (sync_migration)
			defer_compaction(preferred_zone, order);

		cond_resched();
	}

	return NULL;
}
#else
static inline struct page *
__alloc_pages_direct_compact(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, bool sync_migration,
	bool *contended_compaction, bool *deferred_compaction,
	unsigned long *did_some_progress)
{
	return NULL;
}
#endif /* CONFIG_COMPACTION */

/* Perform direct synchronous page reclaim */
static int
__perform_reclaim(gfp_t gfp_mask, unsigned int order, struct zonelist *zonelist,
		  nodemask_t *nodemask)
{
	struct reclaim_state reclaim_state;
	int progress;

	cond_resched();

	/* We now go into synchronous reclaim */
	cpuset_memory_pressure_bump();
	current->flags |= PF_MEMALLOC;
	lockdep_set_current_reclaim_state(gfp_mask);
	reclaim_state.reclaimed_slab = 0;
	current->reclaim_state = &reclaim_state;

	progress = try_to_free_pages(zonelist, order, gfp_mask, nodemask);

	current->reclaim_state = NULL;
	lockdep_clear_current_reclaim_state();
	current->flags &= ~PF_MEMALLOC;

	cond_resched();

	return progress;
}

/* The really slow allocator path where we enter direct reclaim */
static inline struct page *
__alloc_pages_direct_reclaim(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, int alloc_flags, struct zone *preferred_zone,
	int migratetype, unsigned long *did_some_progress)
{
	struct page *page = NULL;
	bool drained = false;

	*did_some_progress = __perform_reclaim(gfp_mask, order, zonelist,
					       nodemask);
	if (unlikely(!(*did_some_progress)))
		return NULL;

	/* After successful reclaim, reconsider all zones for allocation */
	if (IS_ENABLED(CONFIG_NUMA))
		zlc_clear_zones_full(zonelist);

retry:
	page = get_page_from_freelist(gfp_mask, nodemask, order,
					zonelist, high_zoneidx,
					alloc_flags & ~ALLOC_NO_WATERMARKS,
					preferred_zone, migratetype);

	/*
	 * If an allocation failed after direct reclaim, it could be because
	 * pages are pinned on the per-cpu lists. Drain them and try again
	 */
	if (!page && !drained) {
		drain_all_pages();
		drained = true;
		goto retry;
	}

	return page;
}

/*
 * This is called in the allocator slow-path if the allocation request is of
 * sufficient urgency to ignore watermarks and take other desperate measures
 */
static inline struct page *
__alloc_pages_high_priority(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	struct page *page;

	do {
		page = get_page_from_freelist(gfp_mask, nodemask, order,
			zonelist, high_zoneidx, ALLOC_NO_WATERMARKS,
			preferred_zone, migratetype);

		if (!page && gfp_mask & __GFP_NOFAIL)
			wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/50);
	} while (!page && (gfp_mask & __GFP_NOFAIL));

	return page;
}

static void prepare_slowpath(gfp_t gfp_mask, unsigned int order,
			     struct zonelist *zonelist,
			     enum zone_type high_zoneidx,
			     struct zone *preferred_zone)
{
	struct zoneref *z;
	struct zone *zone;

	for_each_zone_zonelist(zone, z, zonelist, high_zoneidx) {
		if (!(gfp_mask & __GFP_NO_KSWAPD))
			wakeup_kswapd(zone, order, zone_idx(preferred_zone));
		/*
		 * Only reset the batches of zones that were actually
		 * considered in the fast path, we don't want to
		 * thrash fairness information for zones that are not
		 * actually part of this zonelist's round-robin cycle.
		 */
		if (!zone_local(preferred_zone, zone))
			continue;
		mod_zone_page_state(zone, NR_ALLOC_BATCH,
				    high_wmark_pages(zone) -
				    low_wmark_pages(zone) -
				    zone_page_state(zone, NR_ALLOC_BATCH));
	}
}

static inline int
gfp_to_alloc_flags(gfp_t gfp_mask)
{
	int alloc_flags = ALLOC_WMARK_MIN | ALLOC_CPUSET;
	const gfp_t wait = gfp_mask & __GFP_WAIT;

	/* __GFP_HIGH is assumed to be the same as ALLOC_HIGH to save a branch. */
	BUILD_BUG_ON(__GFP_HIGH != (__force gfp_t) ALLOC_HIGH);

	/*
	 * The caller may dip into page reserves a bit more if the caller
	 * cannot run direct reclaim, or if the caller has realtime scheduling
	 * policy or is asking for __GFP_HIGH memory.  GFP_ATOMIC requests will
	 * set both ALLOC_HARDER (!wait) and ALLOC_HIGH (__GFP_HIGH).
	 */
	alloc_flags |= (__force int) (gfp_mask & __GFP_HIGH);

	if (!wait) {
		/*
		 * Not worth trying to allocate harder for
		 * __GFP_NOMEMALLOC even if it can't schedule.
		 */
		if  (!(gfp_mask & __GFP_NOMEMALLOC))
			alloc_flags |= ALLOC_HARDER;
		/*
		 * Ignore cpuset if GFP_ATOMIC (!wait) rather than fail alloc.
		 * See also cpuset_zone_allowed() comment in kernel/cpuset.c.
		 */
		alloc_flags &= ~ALLOC_CPUSET;
	} else if (unlikely(rt_task(current)) && !in_interrupt())
		alloc_flags |= ALLOC_HARDER;

	if (likely(!(gfp_mask & __GFP_NOMEMALLOC))) {
		if (gfp_mask & __GFP_MEMALLOC)
			alloc_flags |= ALLOC_NO_WATERMARKS;
		else if (in_serving_softirq() && (current->flags & PF_MEMALLOC))
			alloc_flags |= ALLOC_NO_WATERMARKS;
		else if (!in_interrupt() &&
				((current->flags & PF_MEMALLOC) ||
				 unlikely(test_thread_flag(TIF_MEMDIE))))
			alloc_flags |= ALLOC_NO_WATERMARKS;
	}
#ifdef CONFIG_CMA
	if (allocflags_to_migratetype(gfp_mask) == MIGRATE_MOVABLE)
		alloc_flags |= ALLOC_CMA;
#endif
	return alloc_flags;
}

bool gfp_pfmemalloc_allowed(gfp_t gfp_mask)
{
	return !!(gfp_to_alloc_flags(gfp_mask) & ALLOC_NO_WATERMARKS);
}

static inline struct page *
__alloc_pages_slowpath(gfp_t gfp_mask, unsigned int order,
	struct zonelist *zonelist, enum zone_type high_zoneidx,
	nodemask_t *nodemask, struct zone *preferred_zone,
	int migratetype)
{
	const gfp_t wait = gfp_mask & __GFP_WAIT;
	struct page *page = NULL;
	int alloc_flags;
	unsigned long pages_reclaimed = 0;
	unsigned long did_some_progress;
	bool sync_migration = false;
	bool deferred_compaction = false;
	bool contended_compaction = false;

	/*
	 * In the slowpath, we sanity check order to avoid ever trying to
	 * reclaim >= MAX_ORDER areas which will never succeed. Callers may
	 * be using allocators in order of preference for an area that is
	 * too large.
	 */
	if (order >= MAX_ORDER) {
		WARN_ON_ONCE(!(gfp_mask & __GFP_NOWARN));
		return NULL;
	}

	/*
	 * GFP_THISNODE (meaning __GFP_THISNODE, __GFP_NORETRY and
	 * __GFP_NOWARN set) should not cause reclaim since the subsystem
	 * (f.e. slab) using GFP_THISNODE may choose to trigger reclaim
	 * using a larger set of nodes after it has established that the
	 * allowed per node queues are empty and that nodes are
	 * over allocated.
	 */
	if (gfp_thisnode_allocation(gfp_mask))
		goto nopage;

restart:
	prepare_slowpath(gfp_mask, order, zonelist,
			 high_zoneidx, preferred_zone);

	/*
	 * OK, we're below the kswapd watermark and have kicked background
	 * reclaim. Now things get more complex, so set up alloc_flags according
	 * to how we want to proceed.
	 */
	alloc_flags = gfp_to_alloc_flags(gfp_mask);

	/*
	 * Find the true preferred zone if the allocation is unconstrained by
	 * cpusets.
	 */
	if (!(alloc_flags & ALLOC_CPUSET) && !nodemask)
		first_zones_zonelist(zonelist, high_zoneidx, NULL,
					&preferred_zone);

rebalance:
	/* This is the last chance, in general, before the goto nopage. */
	page = get_page_from_freelist(gfp_mask, nodemask, order, zonelist,
			high_zoneidx, alloc_flags & ~ALLOC_NO_WATERMARKS,
			preferred_zone, migratetype);
	if (page)
		goto got_pg;

	/* Allocate without watermarks if the context allows */
	if (alloc_flags & ALLOC_NO_WATERMARKS) {
		/*
		 * Ignore mempolicies if ALLOC_NO_WATERMARKS on the grounds
		 * the allocation is high priority and these type of
		 * allocations are system rather than user orientated
		 */
		zonelist = node_zonelist(numa_node_id(), gfp_mask);

		page = __alloc_pages_high_priority(gfp_mask, order,
				zonelist, high_zoneidx, nodemask,
				preferred_zone, migratetype);
		if (page) {
			goto got_pg;
		}
	}

	/* Atomic allocations - we can't balance anything */
	if (!wait)
		goto nopage;

	/* Avoid recursion of direct reclaim */
	if (current->flags & PF_MEMALLOC)
		goto nopage;

	/* Avoid allocations with no watermarks from looping endlessly */
	if (test_thread_flag(TIF_MEMDIE) && !(gfp_mask & __GFP_NOFAIL))
		goto nopage;

	/*
	 * Try direct compaction. The first pass is asynchronous. Subsequent
	 * attempts after direct reclaim are synchronous
	 */
	page = __alloc_pages_direct_compact(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, sync_migration,
					&contended_compaction,
					&deferred_compaction,
					&did_some_progress);
	if (page)
		goto got_pg;
	sync_migration = true;

	/*
	 * If compaction is deferred for high-order allocations, it is because
	 * sync compaction recently failed. In this is the case and the caller
	 * requested a movable allocation that does not heavily disrupt the
	 * system then fail the allocation instead of entering direct reclaim.
	 */
	if ((deferred_compaction || contended_compaction) &&
						(gfp_mask & __GFP_NO_KSWAPD))
		goto nopage;

	/* Try direct reclaim and then allocating */
	page = __alloc_pages_direct_reclaim(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, &did_some_progress);
	if (page)
		goto got_pg;

	/*
	 * If we failed to make any progress reclaiming, then we are
	 * running out of options and have to consider going OOM
	 */
	if (!did_some_progress) {
		if (oom_gfp_allowed(gfp_mask)) {
			if (oom_killer_disabled)
				goto nopage;
			/* Coredumps can quickly deplete all memory reserves */
			if ((current->flags & PF_DUMPCORE) &&
			    !(gfp_mask & __GFP_NOFAIL))
				goto nopage;
			page = __alloc_pages_may_oom(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask, preferred_zone,
					migratetype);
			if (page)
				goto got_pg;

			if (!(gfp_mask & __GFP_NOFAIL)) {
				/*
				 * The oom killer is not called for high-order
				 * allocations that may fail, so if no progress
				 * is being made, there are no other options and
				 * retrying is unlikely to help.
				 */
				if (order > PAGE_ALLOC_COSTLY_ORDER)
					goto nopage;
				/*
				 * The oom killer is not called for lowmem
				 * allocations to prevent needlessly killing
				 * innocent tasks.
				 */
				if (high_zoneidx < ZONE_NORMAL)
					goto nopage;
			}

			goto restart;
		}
	}

	/* Check if we should retry the allocation */
	pages_reclaimed += did_some_progress;
	if (should_alloc_retry(gfp_mask, order, did_some_progress,
						pages_reclaimed)) {
		/* Wait for some write requests to complete then retry */
		wait_iff_congested(preferred_zone, BLK_RW_ASYNC, HZ/50);
		goto rebalance;
	} else {
		/*
		 * High-order allocations do not necessarily loop after
		 * direct reclaim and reclaim/compaction depends on compaction
		 * being called after reclaim so call directly if necessary
		 */
		page = __alloc_pages_direct_compact(gfp_mask, order,
					zonelist, high_zoneidx,
					nodemask,
					alloc_flags, preferred_zone,
					migratetype, sync_migration,
					&contended_compaction,
					&deferred_compaction,
					&did_some_progress);
		if (page)
			goto got_pg;
	}

nopage:
	warn_alloc_failed(gfp_mask, order, NULL);
	return page;
got_pg:
	if (kmemcheck_enabled)
		kmemcheck_pagealloc_alloc(page, order, gfp_mask);

	return page;
}

/*
 * This is the 'heart' of the zoned buddy allocator.
 */
// ARM10C 20140426
// gfp_mask: 0x201200, order: 0,
// zonelist: contig_page_data->node_zonelists, NULL
// ARM10C 20140628
// gfp_mask: 0x201200, order: 0
// zonelist: contig_page_data->node_zonelists, NULL
struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order,
			struct zonelist *zonelist, nodemask_t *nodemask)
{
	// gfp_mask: 0x201200, gfp_zone(0x201200): 0
	enum zone_type high_zoneidx = gfp_zone(gfp_mask);
	// high_zoneidx: ZONE_NORMAL: 0
	struct zone *preferred_zone;
	struct page *page = NULL;
	// gfp_mask: 0x201200
	int migratetype = allocflags_to_migratetype(gfp_mask);
	// migratetype: MIGRATE_UNMOVABLE: 0
	unsigned int cpuset_mems_cookie;
	// ALLOC_WMARK_LOW: 1 ALLOC_CPUSET: 0x40
	int alloc_flags = ALLOC_WMARK_LOW|ALLOC_CPUSET;
	// alloc_flags: 0x41
	struct mem_cgroup *memcg = NULL;

	// gfp_mask: 0x201200, gfp_allowed_mask: 0x1ffff2f
	gfp_mask &= gfp_allowed_mask;
	// gfp_mask: 0x201200

	// gfp_mask: 0x201200
	lockdep_trace_alloc(gfp_mask); // null function

	// gfp_mask: 0x201200, __GFP_WAIT: 0x10u
	might_sleep_if(gfp_mask & __GFP_WAIT);

	// gfp_mask: 0x201200, order: 0, should_fail_alloc_page(0x201200, 0): 0
	if (should_fail_alloc_page(gfp_mask, order))
		return NULL;

	/*
	 * Check the zones suitable for the gfp_mask contain at least one
	 * valid zone. It's possible to have an empty zonelist as a result
	 * of GFP_THISNODE and a memoryless node
	 */
	// zonelist->_zonerefs->zone: (&contig_page_data)->node_zonelists->_zonerefs->zone
	if (unlikely(!zonelist->_zonerefs->zone))
		return NULL;

	/*
	 * Will only have any effect when __GFP_KMEMCG is set.  This is
	 * verified in the (always inline) callee
	 */
	// gfp_mask: 0x201200, &memcg: NULL, order: 0
	// memcg_kmem_newpage_charge(0x201200, NULL, 0): 1
	if (!memcg_kmem_newpage_charge(gfp_mask, &memcg, order))
		return NULL;

retry_cpuset:
	// get_mems_allowed(): 0
	cpuset_mems_cookie = get_mems_allowed();
	// cpuset_mems_cookie: 0

	/* The preferred zone is used for statistics later */
	// zonelist: contig_page_data->node_zonelists, high_zoneidx: ZONE_NORMAL: 0
	// nodemask: NULL, cpuset_current_mems_allowed: node_states[N_HIGH_MEMORY], &preferred_zone
	first_zones_zonelist(zonelist, high_zoneidx,
				nodemask ? : &cpuset_current_mems_allowed,
				&preferred_zone);
	// preferred_zone: (&contig_page_data)->node_zones[0]

	if (!preferred_zone)
		goto out;

#ifdef CONFIG_CMA // CONFIG_CMA=n
	if (allocflags_to_migratetype(gfp_mask) == MIGRATE_MOVABLE)
		alloc_flags |= ALLOC_CMA;
#endif

// 2014/04/26 종료
// 2014/05/10 시작

	/* First allocation attempt */
	// gfp_mask: 0x201200, __GFP_HARDWALL: 0x20000, nodemask: NULL, order: 0
	// zonelist: contig_page_data->node_zonelists, high_zoneidx: ZONE_NORMAL: 0
	// alloc_flags: 0x41, preferred_zone: (&contig_page_data)->node_zones[0]
	// migratetype: MIGRATE_UNMOVABLE: 0
	page = get_page_from_freelist(gfp_mask|__GFP_HARDWALL, nodemask, order,
			zonelist, high_zoneidx, alloc_flags,
			preferred_zone, migratetype);
	// page: migratetype이 MIGRATE_UNMOVABLE인 page

	if (unlikely(!page)) {
		/*
		 * Runtime PM, block IO and its error handling path
		 * can deadlock because I/O on the device might not
		 * complete.
		 */
		gfp_mask = memalloc_noio_flags(gfp_mask);
		page = __alloc_pages_slowpath(gfp_mask, order,
				zonelist, high_zoneidx, nodemask,
				preferred_zone, migratetype);
	}

	trace_mm_page_alloc(page, order, gfp_mask, migratetype);

out:
	/*
	 * When updating a task's mems_allowed, it is possible to race with
	 * parallel threads in such a way that an allocation can fail while
	 * the mask is being updated. If a page allocation is about to fail,
	 * check if the cpuset changed during allocation and if so, retry.
	 */
	// page: migratetype이 MIGRATE_UNMOVABLE인 page, cpuset_mems_cookie: 0
	// put_mems_allowed(0): true
	if (unlikely(!put_mems_allowed(cpuset_mems_cookie) && !page))
		goto retry_cpuset;

	// page: migratetype이 MIGRATE_UNMOVABLE인 page, memcg: NULL, order: 0
	memcg_kmem_commit_charge(page, memcg, order); // null function

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	return page;
}
EXPORT_SYMBOL(__alloc_pages_nodemask);

/*
 * Common helper functions.
 */
// ARM10C 20141101
// PGALLOC_GFP: 0x2084D0, 0
unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order)
{
	struct page *page;

	/*
	 * __get_free_pages() returns a 32-bit address, which cannot represent
	 * a highmem page
	 */
	// gfp_mask: PGALLOC_GFP: 0x2084D0, __GFP_HIGHMEM: 0x02u
	VM_BUG_ON((gfp_mask & __GFP_HIGHMEM) != 0);

	// gfp_mask: PGALLOC_GFP: 0x2084D0, order: 0
	// alloc_pages(PGALLOC_GFP: 0x2084D0, 0): migratetype이 MIGRATE_UNMOVABLE인 page
	page = alloc_pages(gfp_mask, order);
	// page: migratetype이 MIGRATE_UNMOVABLE인 page

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	if (!page)
		return 0;

	// page: migratetype이 MIGRATE_UNMOVABLE인 page
	// page_address(migratetype이 MIGRATE_UNMOVABLE인 page): migratetype이 MIGRATE_UNMOVABLE인 page의 가상주소
	return (unsigned long) page_address(page);
	// return migratetype이 MIGRATE_UNMOVABLE인 page의 가상주소
}
EXPORT_SYMBOL(__get_free_pages);

unsigned long get_zeroed_page(gfp_t gfp_mask)
{
	return __get_free_pages(gfp_mask | __GFP_ZERO, 0);
}
EXPORT_SYMBOL(get_zeroed_page);

// ARM10C 20140329
// page: 0x20000의 해당하는 struct page의 1st page, order: 5
// ARM10C 20140412
// [order: 0] order: 0
// ARM10C 20140419
void __free_pages(struct page *page, unsigned int order)
{
	// page: 0x20000 (pfn)
	if (put_page_testzero(page)) {
		// put_page_testzero(page): 1

		// [order: 5] order: 5
		// [order: 0] order: 0
		if (order == 0)
			// page: 0x20000 (pfn)
			free_hot_cold_page(page, 0);
			// CPU0의 vm_event_states.event[PGFREE] 를 1로 설정함
			// page에 해당하는 pageblock의 migrate flag를 반환함
			// struct page의 index 멤버에 migratetype을 저장함
			// order 0 buddy를 contig_page_data에 추가함
			// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[NR_FREE_PAGES]: 1 로 설정
			// vmstat.c의 vm_stat[NR_FREE_PAGES] 전역 변수에도 1로 설정
		else
			// page: 0x20000의 해당하는 struct page의 1st page
			// order: 5
			__free_pages_ok(page, order);
			// CPU0의 vm_event_states.event[PGFREE] 를 32로 설정함
			// page에 해당하는 pageblock의 migrate flag를 반환함
			// struct page의 index 멤버에 migratetype을 저장함
			// order 5 buddy를 contig_page_data에 추가함
			// (&contig_page_data)->node_zones[ZONE_NORMAL].vm_stat[NR_FREE_PAGES]: 32 로 설정
			// vmstat.c의 vm_stat[NR_FREE_PAGES] 전역 변수에도 32로 설정
	}
}

EXPORT_SYMBOL(__free_pages);

void free_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		VM_BUG_ON(!virt_addr_valid((void *)addr));
		__free_pages(virt_to_page((void *)addr), order);
	}
}

EXPORT_SYMBOL(free_pages);

/*
 * __free_memcg_kmem_pages and free_memcg_kmem_pages will free
 * pages allocated with __GFP_KMEMCG.
 *
 * Those pages are accounted to a particular memcg, embedded in the
 * corresponding page_cgroup. To avoid adding a hit in the allocator to search
 * for that information only to find out that it is NULL for users who have no
 * interest in that whatsoever, we provide these functions.
 *
 * The caller knows better which flags it relies on.
 */
void __free_memcg_kmem_pages(struct page *page, unsigned int order)
{
	memcg_kmem_uncharge_pages(page, order);
	__free_pages(page, order);
}

void free_memcg_kmem_pages(unsigned long addr, unsigned int order)
{
	if (addr != 0) {
		VM_BUG_ON(!virt_addr_valid((void *)addr));
		__free_memcg_kmem_pages(virt_to_page((void *)addr), order);
	}
}

static void *make_alloc_exact(unsigned long addr, unsigned order, size_t size)
{
	if (addr) {
		unsigned long alloc_end = addr + (PAGE_SIZE << order);
		unsigned long used = addr + PAGE_ALIGN(size);

		split_page(virt_to_page((void *)addr), order);
		while (used < alloc_end) {
			free_page(used);
			used += PAGE_SIZE;
		}
	}
	return (void *)addr;
}

/**
 * alloc_pages_exact - allocate an exact number physically-contiguous pages.
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation
 *
 * This function is similar to alloc_pages(), except that it allocates the
 * minimum number of pages to satisfy the request.  alloc_pages() can only
 * allocate memory in power-of-two pages.
 *
 * This function is also limited by MAX_ORDER.
 *
 * Memory allocated by this function must be released by free_pages_exact().
 */
void *alloc_pages_exact(size_t size, gfp_t gfp_mask)
{
	unsigned int order = get_order(size);
	unsigned long addr;

	addr = __get_free_pages(gfp_mask, order);
	return make_alloc_exact(addr, order, size);
}
EXPORT_SYMBOL(alloc_pages_exact);

/**
 * alloc_pages_exact_nid - allocate an exact number of physically-contiguous
 *			   pages on a node.
 * @nid: the preferred node ID where memory should be allocated
 * @size: the number of bytes to allocate
 * @gfp_mask: GFP flags for the allocation
 *
 * Like alloc_pages_exact(), but try to allocate on node nid first before falling
 * back.
 * Note this is not alloc_pages_exact_node() which allocates on a specific node,
 * but is not exact.
 */
void *alloc_pages_exact_nid(int nid, size_t size, gfp_t gfp_mask)
{
	unsigned order = get_order(size);
	struct page *p = alloc_pages_node(nid, gfp_mask, order);
	if (!p)
		return NULL;
	return make_alloc_exact((unsigned long)page_address(p), order, size);
}
EXPORT_SYMBOL(alloc_pages_exact_nid);

/**
 * free_pages_exact - release memory allocated via alloc_pages_exact()
 * @virt: the value returned by alloc_pages_exact.
 * @size: size of allocation, same value as passed to alloc_pages_exact().
 *
 * Release the memory allocated by a previous call to alloc_pages_exact.
 */
void free_pages_exact(void *virt, size_t size)
{
	unsigned long addr = (unsigned long)virt;
	unsigned long end = addr + PAGE_ALIGN(size);

	while (addr < end) {
		free_page(addr);
		addr += PAGE_SIZE;
	}
}
EXPORT_SYMBOL(free_pages_exact);

/**
 * nr_free_zone_pages - count number of pages beyond high watermark
 * @offset: The zone index of the highest zone
 *
 * nr_free_zone_pages() counts the number of counts pages which are beyond the
 * high watermark within all zones at or below a given zone index.  For each
 * zone, the number of pages is calculated as:
 *     managed_pages - high_pages
 */
// ARM10C 20140308
// offset: 0
static unsigned long nr_free_zone_pages(int offset)
{
	struct zoneref *z;
	struct zone *zone;

	/* Just pick one node, since fallback list is circular */
	unsigned long sum = 0;

	// numa_node_id(): 0, GFP_KERNEL: 0xD0
	struct zonelist *zonelist = node_zonelist(numa_node_id(), GFP_KERNEL);
	// zonelist: contig_page_data->node_zonelists

	// zonelist: contig_page_data->node_zonelists, offset: 0
	for_each_zone_zonelist(zone, z, zonelist, offset) {
	// for (z = first_zones_zonelist(contig_page_data->node_zonelists, 0, 0, &zone);
	//          zone; z = next_zones_zonelist(++z, 0, 0, &zone))
		// [1st] z: contig_page_data->node_zonelists->_zonerefs[1]
		// [1st] zone: contig_page_data->node_zones[0]
		// [2nd] z: contig_page_data->node_zonelists->_zonerefs[0]
		// [2nd] zone: contig_page_data->node_zones[1]

		// [1st]: zone->managed_pages: contig_page_data->node_zones[0]->managed_pages: 0x2efd6
		// [2nd]: zone->managed_pages: contig_page_data->node_zones[1]->managed_pages: 0x50800
		unsigned long size = zone->managed_pages;
		// [1st]: size: 0x2efd6
		// [2nd]: size: 0x50800

		// [1st] zone: contig_page_data->node_zones[0]
		// [1st] high_wmark_pages(contig_page_data->node_zones[0]): 0
		// [2st] zone: contig_page_data->node_zones[1]
		// [2st] high_wmark_pages(contig_page_data->node_zones[1]): 0
		unsigned long high = high_wmark_pages(zone);
		// [1st]: high: 0
		// [2nd]: high: 0

		// [1st]: size: 0x2efd6
		// [2nd]: size: 0x50800
		if (size > high)
			// [1st] sum: 0
			// [2nd]: sum: 0x2efd6
			sum += size - high;
			// [1st]: sum: 0x2efd6
			// [2nd]: sum: 0x7f7d6
	}

	// sum: 0x7f7d6
	return sum;
	// return 0x7f7d6
}

/**
 * nr_free_buffer_pages - count number of pages beyond high watermark
 *
 * nr_free_buffer_pages() counts the number of pages which are beyond the high
 * watermark within ZONE_DMA and ZONE_NORMAL.
 */
unsigned long nr_free_buffer_pages(void)
{
	return nr_free_zone_pages(gfp_zone(GFP_USER));
}
EXPORT_SYMBOL_GPL(nr_free_buffer_pages);

/**
 * nr_free_pagecache_pages - count number of pages beyond high watermark
 *
 * nr_free_pagecache_pages() counts the number of pages which are beyond the
 * high watermark within all zones.
 */
// ARM10C 20140308
unsigned long nr_free_pagecache_pages(void)
{
	// GFP_HIGHUSER_MOVABLE: 0x200DA, gfp_zone(GFP_HIGHUSER_MOVABLE): 0
	return nr_free_zone_pages(gfp_zone(GFP_HIGHUSER_MOVABLE));
	// return 0x7f7d6
}

static inline void show_node(struct zone *zone)
{
	if (IS_ENABLED(CONFIG_NUMA))
		printk("Node %d ", zone_to_nid(zone));
}

void si_meminfo(struct sysinfo *val)
{
	val->totalram = totalram_pages;
	val->sharedram = 0;
	val->freeram = global_page_state(NR_FREE_PAGES);
	val->bufferram = nr_blockdev_pages();
	val->totalhigh = totalhigh_pages;
	val->freehigh = nr_free_highpages();
	val->mem_unit = PAGE_SIZE;
}

EXPORT_SYMBOL(si_meminfo);

#ifdef CONFIG_NUMA
void si_meminfo_node(struct sysinfo *val, int nid)
{
	int zone_type;		/* needs to be signed */
	unsigned long managed_pages = 0;
	pg_data_t *pgdat = NODE_DATA(nid);

	for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++)
		managed_pages += pgdat->node_zones[zone_type].managed_pages;
	val->totalram = managed_pages;
	val->freeram = node_page_state(nid, NR_FREE_PAGES);
#ifdef CONFIG_HIGHMEM
	val->totalhigh = pgdat->node_zones[ZONE_HIGHMEM].managed_pages;
	val->freehigh = zone_page_state(&pgdat->node_zones[ZONE_HIGHMEM],
			NR_FREE_PAGES);
#else
	val->totalhigh = 0;
	val->freehigh = 0;
#endif
	val->mem_unit = PAGE_SIZE;
}
#endif

/*
 * Determine whether the node should be displayed or not, depending on whether
 * SHOW_MEM_FILTER_NODES was passed to show_free_areas().
 */
bool skip_free_areas_node(unsigned int flags, int nid)
{
	bool ret = false;
	unsigned int cpuset_mems_cookie;

	if (!(flags & SHOW_MEM_FILTER_NODES))
		goto out;

	do {
		cpuset_mems_cookie = get_mems_allowed();
		ret = !node_isset(nid, cpuset_current_mems_allowed);
	} while (!put_mems_allowed(cpuset_mems_cookie));
out:
	return ret;
}

#define K(x) ((x) << (PAGE_SHIFT-10))

static void show_migration_types(unsigned char type)
{
	static const char types[MIGRATE_TYPES] = {
		[MIGRATE_UNMOVABLE]	= 'U',
		[MIGRATE_RECLAIMABLE]	= 'E',
		[MIGRATE_MOVABLE]	= 'M',
		[MIGRATE_RESERVE]	= 'R',
#ifdef CONFIG_CMA
		[MIGRATE_CMA]		= 'C',
#endif
#ifdef CONFIG_MEMORY_ISOLATION
		[MIGRATE_ISOLATE]	= 'I',
#endif
	};
	char tmp[MIGRATE_TYPES + 1];
	char *p = tmp;
	int i;

	for (i = 0; i < MIGRATE_TYPES; i++) {
		if (type & (1 << i))
			*p++ = types[i];
	}

	*p = '\0';
	printk("(%s) ", tmp);
}

/*
 * Show free area list (used inside shift_scroll-lock stuff)
 * We also calculate the percentage fragmentation. We do this by counting the
 * memory on each free list with the exception of the first item on the list.
 * Suppresses nodes that are not allowed by current's cpuset if
 * SHOW_MEM_FILTER_NODES is passed.
 */
void show_free_areas(unsigned int filter)
{
	int cpu;
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s per-cpu:\n", zone->name);

		for_each_online_cpu(cpu) {
			struct per_cpu_pageset *pageset;

			pageset = per_cpu_ptr(zone->pageset, cpu);

			printk("CPU %4d: hi:%5d, btch:%4d usd:%4d\n",
			       cpu, pageset->pcp.high,
			       pageset->pcp.batch, pageset->pcp.count);
		}
	}

	printk("active_anon:%lu inactive_anon:%lu isolated_anon:%lu\n"
		" active_file:%lu inactive_file:%lu isolated_file:%lu\n"
		" unevictable:%lu"
		" dirty:%lu writeback:%lu unstable:%lu\n"
		" free:%lu slab_reclaimable:%lu slab_unreclaimable:%lu\n"
		" mapped:%lu shmem:%lu pagetables:%lu bounce:%lu\n"
		" free_cma:%lu\n",
		global_page_state(NR_ACTIVE_ANON),
		global_page_state(NR_INACTIVE_ANON),
		global_page_state(NR_ISOLATED_ANON),
		global_page_state(NR_ACTIVE_FILE),
		global_page_state(NR_INACTIVE_FILE),
		global_page_state(NR_ISOLATED_FILE),
		global_page_state(NR_UNEVICTABLE),
		global_page_state(NR_FILE_DIRTY),
		global_page_state(NR_WRITEBACK),
		global_page_state(NR_UNSTABLE_NFS),
		global_page_state(NR_FREE_PAGES),
		global_page_state(NR_SLAB_RECLAIMABLE),
		global_page_state(NR_SLAB_UNRECLAIMABLE),
		global_page_state(NR_FILE_MAPPED),
		global_page_state(NR_SHMEM),
		global_page_state(NR_PAGETABLE),
		global_page_state(NR_BOUNCE),
		global_page_state(NR_FREE_CMA_PAGES));

	for_each_populated_zone(zone) {
		int i;

		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s"
			" free:%lukB"
			" min:%lukB"
			" low:%lukB"
			" high:%lukB"
			" active_anon:%lukB"
			" inactive_anon:%lukB"
			" active_file:%lukB"
			" inactive_file:%lukB"
			" unevictable:%lukB"
			" isolated(anon):%lukB"
			" isolated(file):%lukB"
			" present:%lukB"
			" managed:%lukB"
			" mlocked:%lukB"
			" dirty:%lukB"
			" writeback:%lukB"
			" mapped:%lukB"
			" shmem:%lukB"
			" slab_reclaimable:%lukB"
			" slab_unreclaimable:%lukB"
			" kernel_stack:%lukB"
			" pagetables:%lukB"
			" unstable:%lukB"
			" bounce:%lukB"
			" free_cma:%lukB"
			" writeback_tmp:%lukB"
			" pages_scanned:%lu"
			" all_unreclaimable? %s"
			"\n",
			zone->name,
			K(zone_page_state(zone, NR_FREE_PAGES)),
			K(min_wmark_pages(zone)),
			K(low_wmark_pages(zone)),
			K(high_wmark_pages(zone)),
			K(zone_page_state(zone, NR_ACTIVE_ANON)),
			K(zone_page_state(zone, NR_INACTIVE_ANON)),
			K(zone_page_state(zone, NR_ACTIVE_FILE)),
			K(zone_page_state(zone, NR_INACTIVE_FILE)),
			K(zone_page_state(zone, NR_UNEVICTABLE)),
			K(zone_page_state(zone, NR_ISOLATED_ANON)),
			K(zone_page_state(zone, NR_ISOLATED_FILE)),
			K(zone->present_pages),
			K(zone->managed_pages),
			K(zone_page_state(zone, NR_MLOCK)),
			K(zone_page_state(zone, NR_FILE_DIRTY)),
			K(zone_page_state(zone, NR_WRITEBACK)),
			K(zone_page_state(zone, NR_FILE_MAPPED)),
			K(zone_page_state(zone, NR_SHMEM)),
			K(zone_page_state(zone, NR_SLAB_RECLAIMABLE)),
			K(zone_page_state(zone, NR_SLAB_UNRECLAIMABLE)),
			zone_page_state(zone, NR_KERNEL_STACK) *
				THREAD_SIZE / 1024,
			K(zone_page_state(zone, NR_PAGETABLE)),
			K(zone_page_state(zone, NR_UNSTABLE_NFS)),
			K(zone_page_state(zone, NR_BOUNCE)),
			K(zone_page_state(zone, NR_FREE_CMA_PAGES)),
			K(zone_page_state(zone, NR_WRITEBACK_TEMP)),
			zone->pages_scanned,
			(!zone_reclaimable(zone) ? "yes" : "no")
			);
		printk("lowmem_reserve[]:");
		for (i = 0; i < MAX_NR_ZONES; i++)
			printk(" %lu", zone->lowmem_reserve[i]);
		printk("\n");
	}

	for_each_populated_zone(zone) {
		unsigned long nr[MAX_ORDER], flags, order, total = 0;
		unsigned char types[MAX_ORDER];

		if (skip_free_areas_node(filter, zone_to_nid(zone)))
			continue;
		show_node(zone);
		printk("%s: ", zone->name);

		spin_lock_irqsave(&zone->lock, flags);
		for (order = 0; order < MAX_ORDER; order++) {
			struct free_area *area = &zone->free_area[order];
			int type;

			nr[order] = area->nr_free;
			total += nr[order] << order;

			types[order] = 0;
			for (type = 0; type < MIGRATE_TYPES; type++) {
				if (!list_empty(&area->free_list[type]))
					types[order] |= 1 << type;
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);
		for (order = 0; order < MAX_ORDER; order++) {
			printk("%lu*%lukB ", nr[order], K(1UL) << order);
			if (nr[order])
				show_migration_types(types[order]);
		}
		printk("= %lukB\n", K(total));
	}

	hugetlb_show_meminfo();

	printk("%ld total pagecache pages\n", global_page_state(NR_FILE_PAGES));

	show_swap_cache_info();
}

// ARM10C 20140308
// zone: contig_page_data->node_zones[1]
// zonelist->_zonerefs[0]: contig_page_data->node_zonelists[0]._zonerefs[0]
// zone: contig_page_data->node_zones[0]
// zonelist->_zonerefs[0]: contig_page_data->node_zonelists[0]._zonerefs[1]
static void zoneref_set_zone(struct zone *zone, struct zoneref *zoneref)
{
	// zoneref->zone: contig_page_data->node_zonelists[0]._zonerefs[0].zone
	// zoneref->zone: contig_page_data->node_zonelists[0]._zonerefs[1].zone
	zoneref->zone = zone;
	// zoneref->zone: contig_page_data->node_zonelists[0]._zonerefs[0].zone: contig_page_data->node_zones[1]
	// zoneref->zone: contig_page_data->node_zonelists[0]._zonerefs[1].zone: contig_page_data->node_zones[0]

	// zoneref->zone_idx: contig_page_data->node_zonelists[0]._zonerefs[0].zone_idx
	// zone_idx(contig_page_data->node_zones[1]): 1
	// zoneref->zone_idx: contig_page_data->node_zonelists[0]._zonerefs[1].zone_idx
	// zone_idx(contig_page_data->node_zones[0]): 0
	zoneref->zone_idx = zone_idx(zone);
	// zoneref->zone_idx: contig_page_data->node_zonelists[0]._zonerefs[0].zone_idx: 1
	// zoneref->zone_idx: contig_page_data->node_zonelists[0]._zonerefs[1].zone_idx: 0
}

/*
 * Builds allocation fallback zone lists.
 *
 * Add all populated zones of a node to the zonelist.
 */
// ARM10C 20140308
// pgdat: &contig_page_data, zonelist: contig_page_data->node_zonelists[0], 0
static int build_zonelists_node(pg_data_t *pgdat, struct zonelist *zonelist,
				int nr_zones)
{
	struct zone *zone;
	// MAX_NR_ZONES: 3
	enum zone_type zone_type = MAX_NR_ZONES;
	// zone_type: __MAX_NR_ZONES (3)

	do {
		zone_type--;
		// zone_type: ZONE_MOVABLE(2)
		// zone_type: ZONE_HIGHMEM(1)
		// zone_type: ZONE_NORMAL(0)

		// pgdat->node_zones: contig_page_data->node_zones
		zone = pgdat->node_zones + zone_type;
		// zone: contig_page_data->node_zones[2]
		// zone: contig_page_data->node_zones[1]
		// zone: contig_page_data->node_zones[0]

		// populated_zone(contig_page_data->node_zones[2]): 0
		// populated_zone(contig_page_data->node_zones[1]): 1
		// populated_zone(contig_page_data->node_zones[0]): 1
		if (populated_zone(zone)) {
			// nr_zones: 0
			// zone: contig_page_data->node_zones[1]
			// zonelist->_zonerefs[0]: contig_page_data->node_zonelists[0]._zonerefs[0]
			// nr_zones: 1
			// zone: contig_page_data->node_zones[0]
			// zonelist->_zonerefs[0]: contig_page_data->node_zonelists[0]._zonerefs[1]
			zoneref_set_zone(zone,
				&zonelist->_zonerefs[nr_zones++]);
			// nr_zones: 1
			// nr_zones: 2

			// zone_type: ZONE_HIGHMEM(1)
			// zone_type: ZONE_NORMAL(0)
			check_highest_zone(zone_type); // null function
		}
	} while (zone_type);

	// nr_zones: 2
	return nr_zones;
	// return 2
}


/*
 *  zonelist_order:
 *  0 = automatic detection of better ordering.
 *  1 = order by ([node] distance, -zonetype)
 *  2 = order by (-zonetype, [node] distance)
 *
 *  If not NUMA, ZONELIST_ORDER_ZONE and ZONELIST_ORDER_NODE will create
 *  the same zonelist. So only NUMA can configure this param.
 */
#define ZONELIST_ORDER_DEFAULT  0
#define ZONELIST_ORDER_NODE     1
// ARM10C 20140308
#define ZONELIST_ORDER_ZONE     2

/* zonelist order in the kernel.
 * set_zonelist_order() will set this to NODE or ZONE.
 */
// ARM10C 20140308
// current_zonelist_order: 2
static int current_zonelist_order = ZONELIST_ORDER_DEFAULT;
static char zonelist_order_name[3][8] = {"Default", "Node", "Zone"};


#ifdef CONFIG_NUMA // CONFIG_NUMA=n
/* The value user specified ....changed by config */
static int user_zonelist_order = ZONELIST_ORDER_DEFAULT;
/* string for sysctl */
#define NUMA_ZONELIST_ORDER_LEN	16
char numa_zonelist_order[16] = "default";

/*
 * interface for configure zonelist ordering.
 * command line option "numa_zonelist_order"
 *	= "[dD]efault	- default, automatic configuration.
 *	= "[nN]ode 	- order by node locality, then by zone within node
 *	= "[zZ]one      - order by zone, then by locality within zone
 */

static int __parse_numa_zonelist_order(char *s)
{
	if (*s == 'd' || *s == 'D') {
		user_zonelist_order = ZONELIST_ORDER_DEFAULT;
	} else if (*s == 'n' || *s == 'N') {
		user_zonelist_order = ZONELIST_ORDER_NODE;
	} else if (*s == 'z' || *s == 'Z') {
		user_zonelist_order = ZONELIST_ORDER_ZONE;
	} else {
		printk(KERN_WARNING
			"Ignoring invalid numa_zonelist_order value:  "
			"%s\n", s);
		return -EINVAL;
	}
	return 0;
}

static __init int setup_numa_zonelist_order(char *s)
{
	int ret;

	if (!s)
		return 0;

	ret = __parse_numa_zonelist_order(s);
	if (ret == 0)
		strlcpy(numa_zonelist_order, s, NUMA_ZONELIST_ORDER_LEN);

	return ret;
}
early_param("numa_zonelist_order", setup_numa_zonelist_order);

/*
 * sysctl handler for numa_zonelist_order
 */
int numa_zonelist_order_handler(ctl_table *table, int write,
		void __user *buffer, size_t *length,
		loff_t *ppos)
{
	char saved_string[NUMA_ZONELIST_ORDER_LEN];
	int ret;
	static DEFINE_MUTEX(zl_order_mutex);

	mutex_lock(&zl_order_mutex);
	if (write) {
		if (strlen((char *)table->data) >= NUMA_ZONELIST_ORDER_LEN) {
			ret = -EINVAL;
			goto out;
		}
		strcpy(saved_string, (char *)table->data);
	}
	ret = proc_dostring(table, write, buffer, length, ppos);
	if (ret)
		goto out;
	if (write) {
		int oldval = user_zonelist_order;

		ret = __parse_numa_zonelist_order((char *)table->data);
		if (ret) {
			/*
			 * bogus value.  restore saved string
			 */
			strncpy((char *)table->data, saved_string,
				NUMA_ZONELIST_ORDER_LEN);
			user_zonelist_order = oldval;
		} else if (oldval != user_zonelist_order) {
			mutex_lock(&zonelists_mutex);
			build_all_zonelists(NULL, NULL);
			mutex_unlock(&zonelists_mutex);
		}
	}
out:
	mutex_unlock(&zl_order_mutex);
	return ret;
}


#define MAX_NODE_LOAD (nr_online_nodes)
static int node_load[MAX_NUMNODES];

/**
 * find_next_best_node - find the next node that should appear in a given node's fallback list
 * @node: node whose fallback list we're appending
 * @used_node_mask: nodemask_t of already used nodes
 *
 * We use a number of factors to determine which is the next node that should
 * appear on a given node's fallback list.  The node should not have appeared
 * already in @node's fallback list, and it should be the next closest node
 * according to the distance array (which contains arbitrary distance values
 * from each node to each node in the system), and should also prefer nodes
 * with no CPUs, since presumably they'll have very little allocation pressure
 * on them otherwise.
 * It returns -1 if no node is found.
 */
static int find_next_best_node(int node, nodemask_t *used_node_mask)
{
	int n, val;
	int min_val = INT_MAX;
	int best_node = NUMA_NO_NODE;
	const struct cpumask *tmp = cpumask_of_node(0);

	/* Use the local node if we haven't already */
	if (!node_isset(node, *used_node_mask)) {
		node_set(node, *used_node_mask);
		return node;
	}

	for_each_node_state(n, N_MEMORY) {

		/* Don't want a node to appear more than once */
		if (node_isset(n, *used_node_mask))
			continue;

		/* Use the distance array to find the distance */
		val = node_distance(node, n);

		/* Penalize nodes under us ("prefer the next node") */
		val += (n < node);

		/* Give preference to headless and unused nodes */
		tmp = cpumask_of_node(n);
		if (!cpumask_empty(tmp))
			val += PENALTY_FOR_NODE_WITH_CPUS;

		/* Slight preference for less loaded node */
		val *= (MAX_NODE_LOAD*MAX_NUMNODES);
		val += node_load[n];

		if (val < min_val) {
			min_val = val;
			best_node = n;
		}
	}

	if (best_node >= 0)
		node_set(best_node, *used_node_mask);

	return best_node;
}


/*
 * Build zonelists ordered by node and zones within node.
 * This results in maximum locality--normal zone overflows into local
 * DMA zone, if any--but risks exhausting DMA zone.
 */
static void build_zonelists_in_node_order(pg_data_t *pgdat, int node)
{
	int j;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[0];
	for (j = 0; zonelist->_zonerefs[j].zone != NULL; j++)
		;
	j = build_zonelists_node(NODE_DATA(node), zonelist, j);
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/*
 * Build gfp_thisnode zonelists
 */
static void build_thisnode_zonelists(pg_data_t *pgdat)
{
	int j;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[1];
	j = build_zonelists_node(pgdat, zonelist, 0);
	zonelist->_zonerefs[j].zone = NULL;
	zonelist->_zonerefs[j].zone_idx = 0;
}

/*
 * Build zonelists ordered by zone and nodes within zones.
 * This results in conserving DMA zone[s] until all Normal memory is
 * exhausted, but results in overflowing to remote node while memory
 * may still exist in local DMA zone.
 */
static int node_order[MAX_NUMNODES];

static void build_zonelists_in_zone_order(pg_data_t *pgdat, int nr_nodes)
{
	int pos, j, node;
	int zone_type;		/* needs to be signed */
	struct zone *z;
	struct zonelist *zonelist;

	zonelist = &pgdat->node_zonelists[0];
	pos = 0;
	for (zone_type = MAX_NR_ZONES - 1; zone_type >= 0; zone_type--) {
		for (j = 0; j < nr_nodes; j++) {
			node = node_order[j];
			z = &NODE_DATA(node)->node_zones[zone_type];
			if (populated_zone(z)) {
				zoneref_set_zone(z,
					&zonelist->_zonerefs[pos++]);
				check_highest_zone(zone_type);
			}
		}
	}
	zonelist->_zonerefs[pos].zone = NULL;
	zonelist->_zonerefs[pos].zone_idx = 0;
}

static int default_zonelist_order(void)
{
	int nid, zone_type;
	unsigned long low_kmem_size, total_size;
	struct zone *z;
	int average_size;
	/*
	 * ZONE_DMA and ZONE_DMA32 can be very small area in the system.
	 * If they are really small and used heavily, the system can fall
	 * into OOM very easily.
	 * This function detect ZONE_DMA/DMA32 size and configures zone order.
	 */
	/* Is there ZONE_NORMAL ? (ex. ppc has only DMA zone..) */
	low_kmem_size = 0;
	total_size = 0;
	for_each_online_node(nid) {
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				if (zone_type < ZONE_NORMAL)
					low_kmem_size += z->managed_pages;
				total_size += z->managed_pages;
			} else if (zone_type == ZONE_NORMAL) {
				/*
				 * If any node has only lowmem, then node order
				 * is preferred to allow kernel allocations
				 * locally; otherwise, they can easily infringe
				 * on other nodes when there is an abundance of
				 * lowmem available to allocate from.
				 */
				return ZONELIST_ORDER_NODE;
			}
		}
	}
	if (!low_kmem_size ||  /* there are no DMA area. */
	    low_kmem_size > total_size/2) /* DMA/DMA32 is big. */
		return ZONELIST_ORDER_NODE;
	/*
	 * look into each node's config.
	 * If there is a node whose DMA/DMA32 memory is very big area on
	 * local memory, NODE_ORDER may be suitable.
	 */
	average_size = total_size /
				(nodes_weight(node_states[N_MEMORY]) + 1);
	for_each_online_node(nid) {
		low_kmem_size = 0;
		total_size = 0;
		for (zone_type = 0; zone_type < MAX_NR_ZONES; zone_type++) {
			z = &NODE_DATA(nid)->node_zones[zone_type];
			if (populated_zone(z)) {
				if (zone_type < ZONE_NORMAL)
					low_kmem_size += z->present_pages;
				total_size += z->present_pages;
			}
		}
		if (low_kmem_size &&
		    total_size > average_size && /* ignore small node */
		    low_kmem_size > total_size * 70/100)
			return ZONELIST_ORDER_NODE;
	}
	return ZONELIST_ORDER_ZONE;
}

static void set_zonelist_order(void)
{
	if (user_zonelist_order == ZONELIST_ORDER_DEFAULT)
		current_zonelist_order = default_zonelist_order();
	else
		current_zonelist_order = user_zonelist_order;
}

static void build_zonelists(pg_data_t *pgdat)
{
	int j, node, load;
	enum zone_type i;
	nodemask_t used_mask;
	int local_node, prev_node;
	struct zonelist *zonelist;
	int order = current_zonelist_order;

	/* initialize zonelists */
	for (i = 0; i < MAX_ZONELISTS; i++) {
		zonelist = pgdat->node_zonelists + i;
		zonelist->_zonerefs[0].zone = NULL;
		zonelist->_zonerefs[0].zone_idx = 0;
	}

	/* NUMA-aware ordering of nodes */
	local_node = pgdat->node_id;
	load = nr_online_nodes;
	prev_node = local_node;
	nodes_clear(used_mask);

	memset(node_order, 0, sizeof(node_order));
	j = 0;

	while ((node = find_next_best_node(local_node, &used_mask)) >= 0) {
		/*
		 * We don't want to pressure a particular node.
		 * So adding penalty to the first node in same
		 * distance group to make it round-robin.
		 */
		if (node_distance(local_node, node) !=
		    node_distance(local_node, prev_node))
			node_load[node] = load;

		prev_node = node;
		load--;
		if (order == ZONELIST_ORDER_NODE)
			build_zonelists_in_node_order(pgdat, node);
		else
			node_order[j++] = node;	/* remember order */
	}

	if (order == ZONELIST_ORDER_ZONE) {
		/* calculate node order -- i.e., DMA last! */
		build_zonelists_in_zone_order(pgdat, j);
	}

	build_thisnode_zonelists(pgdat);
}

/* Construct the zonelist performance cache - see further mmzone.h */
static void build_zonelist_cache(pg_data_t *pgdat)
{
	struct zonelist *zonelist;
	struct zonelist_cache *zlc;
	struct zoneref *z;

	zonelist = &pgdat->node_zonelists[0];
	zonelist->zlcache_ptr = zlc = &zonelist->zlcache;
	bitmap_zero(zlc->fullzones, MAX_ZONES_PER_ZONELIST);
	for (z = zonelist->_zonerefs; z->zone; z++)
		zlc->z_to_n[z - zonelist->_zonerefs] = zonelist_node_idx(z);
}

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
/*
 * Return node id of node used for "local" allocations.
 * I.e., first node id of first zone in arg node's generic zonelist.
 * Used for initializing percpu 'numa_mem', which is used primarily
 * for kernel allocations, so use GFP_KERNEL flags to locate zonelist.
 */
int local_memory_node(int node)
{
	struct zone *zone;

	(void)first_zones_zonelist(node_zonelist(node, GFP_KERNEL),
				   gfp_zone(GFP_KERNEL),
				   NULL,
				   &zone);
	return zone->node;
}
#endif

#else	/* CONFIG_NUMA */

// ARM10C 20140308
static void set_zonelist_order(void)
{
	// ZONELIST_ORDER_ZONE: 2
	current_zonelist_order = ZONELIST_ORDER_ZONE;
	// current_zonelist_order: 2
}

// ARM10C 20140308
// pgdat: &contig_page_data
static void build_zonelists(pg_data_t *pgdat)
{
	int node, local_node;
	enum zone_type j;
	struct zonelist *zonelist;

	// pgdat->node_id: contig_page_data->node_id: 0
	local_node = pgdat->node_id;
	// local_node: 0

	// pgdat->node_zonelists: contig_page_data->node_zonelists
	zonelist = &pgdat->node_zonelists[0];
	// zonelist: contig_page_data->node_zonelists[0]

	// pgdat: &contig_page_data
	j = build_zonelists_node(pgdat, zonelist, 0);
	// j: 2

	/*
	 * Now we build the zonelist so that it contains the zones
	 * of all the other nodes.
	 * We don't want to pressure a particular node, so when
	 * building the zones for node N, we make sure that the
	 * zones coming right after the local ones are those from
	 * node N+1 (modulo N)
	 */
	// local_node: 0, MAX_NUMNODES: 1
	for (node = local_node + 1; node < MAX_NUMNODES; node++) {
		if (!node_online(node))
			continue;
		j = build_zonelists_node(NODE_DATA(node), zonelist, j);
	}

	// local_node: 0
	for (node = 0; node < local_node; node++) {
		if (!node_online(node))
			continue;
		j = build_zonelists_node(NODE_DATA(node), zonelist, j);
	}

	// j: 2
	// zonelist->_zonerefs[2].zone: contig_page_data->node_zonelists[0]->_zonerefs[2].zone
	zonelist->_zonerefs[j].zone = NULL;
	// zonelist->_zonerefs[2].zone: contig_page_data->node_zonelists[0]->_zonerefs[2].zone: NULL

	// zonelist->_zonerefs[2].zone_idx: contig_page_data->node_zonelists[0]->_zonerefs[2].zone_id
	zonelist->_zonerefs[j].zone_idx = 0;
	// zonelist->_zonerefs[2].zone_idx: contig_page_data->node_zonelists[0]->_zonerefs[2].zone_id: 0
}

/* non-NUMA variant of zonelist performance cache - just NULL zlcache_ptr */
// ARM10C 20140308
// pgdat: &contig_page_data
static void build_zonelist_cache(pg_data_t *pgdat)
{
	// pgdat->node_zonelists[0].zlcache_ptr: contig_page_data->node_zonelists[0].zlcache_ptr
	pgdat->node_zonelists[0].zlcache_ptr = NULL;
	// pgdat->node_zonelists[0].zlcache_ptr: contig_page_data->node_zonelists[0].zlcache_ptr: NULL
}

#endif	/* CONFIG_NUMA */

/*
 * Boot pageset table. One per cpu which is going to be used for all
 * zones and all nodes. The parameters will be set in such a way
 * that an item put on a list will immediately be handed over to
 * the buddy list. This is safe since pageset manipulation is done
 * with interrupts disabled.
 *
 * The boot_pagesets must be kept even after bootup is complete for
 * unused processors and/or zones. They do play a role for bootstrapping
 * hotplugged processors.
 *
 * zoneinfo_show() and maybe other functions do
 * not check if the processor is online before following the pageset pointer.
 * Other parts of the kernel may not check if the zone is available.
 */
static void setup_pageset(struct per_cpu_pageset *p, unsigned long batch);
// ARM10C 20140111
// __attribute__((section(.data..percpu))) struct per_cpu_pageset boot_pageset
// ARM10C 20140308
static DEFINE_PER_CPU(struct per_cpu_pageset, boot_pageset);
static void setup_zone_pageset(struct zone *zone);

/*
 * Global mutex to protect against size modification of zonelists
 * as well as to serialize pageset setup for the new populated zone.
 */

DEFINE_MUTEX(zonelists_mutex);

/* return values int ....just for stop_machine() */
// ARM10C 20140308
// NULL
static int __build_all_zonelists(void *data)
{
	int nid;
	int cpu;
	// data: NULL
	pg_data_t *self = data;
	// self: NULL

#ifdef CONFIG_NUMA // CONFIG_NUMA=n
	memset(node_load, 0, sizeof(node_load));
#endif

	// self: NULL
	if (self && !node_online(self->node_id)) {
		build_zonelists(self);
		build_zonelist_cache(self);
	}

	for_each_online_node(nid) {
	// for ( (nid) = 0; (nid) == 0; (nid) = 1)
		// nid: 0
		// NODE_DATA(0): (&contig_page_data)
		pg_data_t *pgdat = NODE_DATA(nid);
		// pgdat: &contig_page_data

		build_zonelists(pgdat);

		// pgdat: &contig_page_data
		build_zonelist_cache(pgdat);
	}

	/*
	 * Initialize the boot_pagesets that are going to be used
	 * for bootstrapping processors. The real pagesets for
	 * each zone will be allocated later when the per cpu
	 * allocator is available.
	 *
	 * boot_pagesets are used also for bootstrapping offline
	 * cpus if the system is already booted because the pagesets
	 * are needed to initialize allocators on a specific cpu too.
	 * F.e. the percpu allocator needs the page allocator which
	 * needs the percpu allocator in order to allocate its pagesets
	 * (a chicken-egg dilemma).
	 */
	for_each_possible_cpu(cpu) {
	// for ((cpu) = -1; (cpu) = cpumask_next((cpu), (cpu_possible_mask)), (cpu) < nr_cpu_ids; )
		// cpu: 0, per_cpu(boot_pageset, 0): *(&boot_pageset + __per_cpu_offset[0])
		// cpu: 1, per_cpu(boot_pageset, 1): *(&boot_pageset + __per_cpu_offset[1])
		// cpu: 2, per_cpu(boot_pageset, 2): *(&boot_pageset + __per_cpu_offset[2])
		// cpu: 3, per_cpu(boot_pageset, 3): *(&boot_pageset + __per_cpu_offset[3])
		setup_pageset(&per_cpu(boot_pageset, cpu), 0);
		// boot_pageset의 pcp (per_cpu_pages) 맴버를 설정


#ifdef CONFIG_HAVE_MEMORYLESS_NODES // CONFIG_HAVE_MEMORYLESS_NODES=n
		/*
		 * We now know the "local memory node" for each node--
		 * i.e., the node of the first zone in the generic zonelist.
		 * Set up numa_mem percpu variable for on-line cpus.  During
		 * boot, only the boot cpu should be on-line;  we'll init the
		 * secondary cpus' numa_mem as they come on-line.  During
		 * node/memory hotplug, we'll fixup all on-line cpus.
		 */
		if (cpu_online(cpu))
			set_cpu_numa_mem(cpu, local_memory_node(cpu_to_node(cpu)));
#endif
	}

	return 0;
}

/*
 * Called with zonelists_mutex held always
 * unless system_state == SYSTEM_BOOTING.
 */
// ARM10C 20140308
// NULL, NULL
void __ref build_all_zonelists(pg_data_t *pgdat, struct zone *zone)
{
	set_zonelist_order();

	// system_state: SYSTEM_BOOTING
	if (system_state == SYSTEM_BOOTING) {
		__build_all_zonelists(NULL);
		// contig_page_data의 node_zonelist마다 값 설정
		// 각 cpu core마다 boot_pageset의 pcp (per_cpu_pages) 맴버를 설정

		mminit_verify_zonelist();
		cpuset_init_current_mems_allowed(); // null function
	} else {
#ifdef CONFIG_MEMORY_HOTPLUG // CONFIG_MEMORY_HOTPLUG=n
		if (zone)
			setup_zone_pageset(zone);
#endif
		/* we have to stop all cpus to guarantee there is no user
		   of zonelist */
		stop_machine(__build_all_zonelists, pgdat, NULL);
		/* cpuset refresh routine should be here */
	}

	// nr_free_pagecache_pages(): 0x7f7d6
	vm_total_pages = nr_free_pagecache_pages();
	// vm_total_pages: 0x7f7d6

	/*
	 * Disable grouping by mobility if the number of pages in the
	 * system is too low to allow the mechanism to work. It would be
	 * more accurate, but expensive to check per-zone. This check is
	 * made on memory-hotadd so a system can start with mobility
	 * disabled and enable it later
	 */
	// vm_total_pages: 0x7f7d6, pageblock_nr_pages : 0x400, MIGRATE_TYPES: 4
	if (vm_total_pages < (pageblock_nr_pages * MIGRATE_TYPES))
		page_group_by_mobility_disabled = 1;
	else
		page_group_by_mobility_disabled = 0;
		// page_group_by_mobility_disabled: 0

	// nr_online_nodes: 1, current_zonelist_order: 2, zonelist_order_name[2]: "Zone"
	// page_group_by_mobility_disabled: 0, "off", vm_total_pages: 0x7f7d6
	printk("Built %i zonelists in %s order, mobility grouping %s.  "
		"Total pages: %ld\n",
			nr_online_nodes,
			zonelist_order_name[current_zonelist_order],
			page_group_by_mobility_disabled ? "off" : "on",
			vm_total_pages);
#ifdef CONFIG_NUMA // CONFIG_NUMA=n
	printk("Policy zone: %s\n", zone_names[policy_zone]);
#endif
}

/*
 * Helper functions to size the waitqueue hash table.
 * Essentially these want to choose hash table sizes sufficiently
 * large so that collisions trying to wait on pages are rare.
 * But in fact, the number of active page waitqueues on typical
 * systems is ridiculously low, less than 200. So this is even
 * conservative, even though it seems large.
 *
 * The constant PAGES_PER_WAITQUEUE specifies the ratio of pages to
 * waitqueues, i.e. the size of the waitq table given the number of pages.
 */
#define PAGES_PER_WAITQUEUE	256

#ifndef CONFIG_MEMORY_HOTPLUG	// CONFIG_MEMORY_HOTPLUG = n
// ARM10C 20140111 
// pages = 0x2F800
static inline unsigned long wait_table_hash_nr_entries(unsigned long pages)
{
	unsigned long size = 1;

	//pages = 0x2f800 / 256 = 0x2f8  
	pages /= PAGES_PER_WAITQUEUE;

	while (size < pages)
		// size = 0x400(1024)
		size <<= 1;

	/*
	 * Once we have dozens or even hundreds of threads sleeping
	 * on IO we've got bigger problems than wait queue collision.
	 * Limit the size of the wait table to a reasonable size.
	 */
	// size = 0x400(1024)
	size = min(size, 4096UL);

	//return size = 0x400(1024)
	return max(size, 4UL);
}
#else
/*
 * A zone's size might be changed by hot-add, so it is not possible to determine
 * a suitable size for its wait_table.  So we use the maximum size now.
 *
 * The max wait table size = 4096 x sizeof(wait_queue_head_t).   ie:
 *
 *    i386 (preemption config)    : 4096 x 16 = 64Kbyte.
 *    ia64, x86-64 (no preemption): 4096 x 20 = 80Kbyte.
 *    ia64, x86-64 (preemption)   : 4096 x 24 = 96Kbyte.
 *
 * The maximum entries are prepared when a zone's memory is (512K + 256) pages
 * or more by the traditional way. (See above).  It equals:
 *
 *    i386, x86-64, powerpc(4K page size) : =  ( 2G + 1M)byte.
 *    ia64(16K page size)                 : =  ( 8G + 4M)byte.
 *    powerpc (64K page size)             : =  (32G +16M)byte.
 */
static inline unsigned long wait_table_hash_nr_entries(unsigned long pages)
{
	return 4096UL;
}
#endif

/*
 * This is an integer logarithm so that shifts can be used later
 * to extract the more random high bits from the multiplicative
 * hash function before the remainder is taken.
 */
// ARM10C 20140111
// size = 1024(0x400) 
static inline unsigned long wait_table_bits(unsigned long size)
{
	//size = 1024(0x400), return 10
	return ffz(~size);
}

/*
 * Check if a pageblock contains reserved pages
 */
static int pageblock_is_reserved(unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!pfn_valid_within(pfn) || PageReserved(pfn_to_page(pfn)))
			return 1;
	}
	return 0;
}

/*
 * Mark a number of pageblocks as MIGRATE_RESERVE. The number
 * of blocks reserved is based on min_wmark_pages(zone). The memory within
 * the reserve will tend to store contiguous free pages. Setting min_free_kbytes
 * higher will lead to a bigger reserve which will get freed as contiguous
 * blocks as reclaim kicks in
 */
static void setup_zone_migrate_reserve(struct zone *zone)
{
	unsigned long start_pfn, pfn, end_pfn, block_end_pfn;
	struct page *page;
	unsigned long block_migratetype;
	int reserve;

	/*
	 * Get the start pfn, end pfn and the number of blocks to reserve
	 * We have to be careful to be aligned to pageblock_nr_pages to
	 * make sure that we always check pfn_valid for the first page in
	 * the block.
	 */
	start_pfn = zone->zone_start_pfn;
	end_pfn = zone_end_pfn(zone);
	start_pfn = roundup(start_pfn, pageblock_nr_pages);
	reserve = roundup(min_wmark_pages(zone), pageblock_nr_pages) >>
							pageblock_order;

	/*
	 * Reserve blocks are generally in place to help high-order atomic
	 * allocations that are short-lived. A min_free_kbytes value that
	 * would result in more than 2 reserve blocks for atomic allocations
	 * is assumed to be in place to help anti-fragmentation for the
	 * future allocation of hugepages at runtime.
	 */
	reserve = min(2, reserve);

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		if (!pfn_valid(pfn))
			continue;
		page = pfn_to_page(pfn);

		/* Watch out for overlapping nodes */
		if (page_to_nid(page) != zone_to_nid(zone))
			continue;

		block_migratetype = get_pageblock_migratetype(page);

		/* Only test what is necessary when the reserves are not met */
		if (reserve > 0) {
			/*
			 * Blocks with reserved pages will never free, skip
			 * them.
			 */
			block_end_pfn = min(pfn + pageblock_nr_pages, end_pfn);
			if (pageblock_is_reserved(pfn, block_end_pfn))
				continue;

			/* If this block is reserved, account for it */
			if (block_migratetype == MIGRATE_RESERVE) {
				reserve--;
				continue;
			}

			/* Suitable for reserving if this block is movable */
			if (block_migratetype == MIGRATE_MOVABLE) {
				set_pageblock_migratetype(page,
							MIGRATE_RESERVE);
				move_freepages_block(zone, page,
							MIGRATE_RESERVE);
				reserve--;
				continue;
			}
		}

		/*
		 * If the reserve is met and this is a previous reserved block,
		 * take it back
		 */
		if (block_migratetype == MIGRATE_RESERVE) {
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);
			move_freepages_block(zone, page, MIGRATE_MOVABLE);
		}
	}
}

/*
 * Initially all pages are reserved - free ones are freed
 * up by free_all_bootmem() once the early boot process is
 * done. Non-atomic initialization, single-pass.
 */
// ARM10C 20140118
// size : 0x2F800, nid : 0, j : 0, zone_start_pfn : 0x20000, context : 0
// size : 0x50800, nid : 0, j : 1, zone_start_pfn : 0x4F800, context : 0
void __meminit memmap_init_zone(unsigned long size, int nid, unsigned long zone,
		unsigned long start_pfn, enum memmap_context context)
{
	struct page *page;
	unsigned long end_pfn = start_pfn + size;
	// end_pfn : 0x4F800
	// end_pfn : 0xA0000
	unsigned long pfn;
	struct zone *z;
	
	// highest_memmap_pfn : 0, end_pfn : 0x4F800
	// highest_memmap_pfn : 0, end_pfn : 0xA0000
	if (highest_memmap_pfn < end_pfn - 1)
		highest_memmap_pfn = end_pfn - 1;
		// highest_memmap_pfn : 0x4F7FF
		// highest_memmap_pfn : 0x9FFFF
	
	// z :&contig_page_data.node_zones[0]
	// z :&contig_page_data.node_zones[1]
	z = &NODE_DATA(nid)->node_zones[zone];
	// start_pfn : 0x20000, end_pfn : 0x4F800
	// start_pfn : 0x4F800, end_pfn : 0xA0000
	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		/*
		 * There can be holes in boot-time mem_map[]s
		 * handed to this function.  They do not
		 * exist on hotplugged memory.
		 */
		// context : MEMMAP_EARLY
		if (context == MEMMAP_EARLY) {
			// pfn : 0x20000
			if (!early_pfn_valid(pfn))	// 1이 리턴됨
				continue;
			// pfn : 0x20000, nid : 0
			if (!early_pfn_in_nid(pfn, nid))
				continue;
		}
		// pfn : 0x20000
		// pfn : 0x4F800
		// page : pfn에 해당하는 struct page의 주소
		page = pfn_to_page(pfn);
		// page : ?, zone : 0, nid : 0, pfn : 0x20000
		// page->flags : 0x20000000 (상위 4비트에 pfn에 해당하는 section, node, zone 번호를 저장)
		// page : ?, zone : 1, nid : 0, pfn : 0x4F800
		// page->flags : 0x44000000 (상위 4비트에 pfn에 해당하는 section, node, zone 번호를 저장)
		set_page_links(page, zone, nid, pfn);
		// page : ?, zone : 0, nid : 0, pfn : 0x20000
		// page : ?, zone : 1, nid : 0, pfn : 0x4F800
		// flag에 제대로 설정이 되었는지 확인
		mminit_verify_page_links(page, zone, nid, pfn);
		// page->__count.counter : 1
		init_page_count(page);
		// page->_mapcount.counter : -1
		page_mapcount_reset(page);
		// null 함수
		page_cpupid_reset_last(page);
		// page->flags의 10(PG_reserved)번째 비트를 1로 atomic set
		SetPageReserved(page);
		/*
		 * Mark the block movable so that blocks are reserved for
		 * movable at startup. This will force kernel allocations
		 * to reserve their blocks rather than leaking throughout
		 * the address space during boot when many long-lived
		 * kernel allocations are made. Later some blocks near
		 * the start are marked MIGRATE_RESERVE by
		 * setup_zone_migrate_reserve()
		 *
		 * bitmap is created for zone's valid pfn range. but memmap
		 * can be created for invalid pages (for alignment)
		 * check here not to call set_pageblock_migratetype() against
		 * pfn out of zone.
		 */
		// zone_start_pfn : 0x20000, pfn : 0x20000
		// zone_end_pfn(z) : 0x4F800
		// pageblock_nr_pages : 0x400
		// MIGRATE_MOVABLE : 2
		// zone_start_pfn : 0x4F800, pfn : 0x4F800
		// zone_end_pfn(z) : 0xA0000
		// pageblock_nr_pages : 0x400
		// MIGRATE_MOVABLE : 2
		if ((z->zone_start_pfn <= pfn)
		    && (pfn < zone_end_pfn(z))
		    && !(pfn & (pageblock_nr_pages - 1)))
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);
			// page에 해당하는 &mem_section[0][2]->pageblock_flags의
			// MIGRATE_MOVABLE(2)번 비트를 1로 설정
			// page에 해당하는 &mem_section[0][4]->pageblock_flags의
		// set_pageblock_migratetype은 1024번째 page(pageblock)가 올 때마다 수행됨
		
		// page->lru 초기화
		INIT_LIST_HEAD(&page->lru);
#ifdef WANT_PAGE_VIRTUAL	// N
		/* The shift won't overflow because ZONE_NORMAL is below 4G. */
		if (!is_highmem_idx(zone))
			set_page_address(page, __va(pfn << PAGE_SHIFT));
#endif
	}
}

// ARM10C 20140111 
static void __meminit zone_init_free_lists(struct zone *zone)
{
	int order, t;
	//  MAX_ORDER = 11
	//  MIGRATE_TYPES = 4
	/*for (order = 0; order < MAX_ORDER; order++) \
		for (t = 0; t < MIGRATE_TYPES; t++)*/
	for_each_migratetype_order(order, t) {
		INIT_LIST_HEAD(&zone->free_area[order].free_list[t]);
		zone->free_area[order].nr_free = 0;
	}
}

#ifndef __HAVE_ARCH_MEMMAP_INIT
// ARM10C 20140118
#define memmap_init(size, nid, zone, start_pfn) \
	memmap_init_zone((size), (nid), (zone), (start_pfn), MEMMAP_EARLY)	// MEMMAP_EARLY : 0
#endif

// ARM10C 20140111
// ARM10C 20150912
// zone: &(&contig_page_data)->node_zones[0]
static int __meminit zone_batchsize(struct zone *zone)
{
#ifdef CONFIG_MMU	// CONFIG_MMU = y
	int batch;

	/*
	 * The per-cpu-pages pools are set to around 1000th of the
	 * size of the zone.  But no more than 1/2 of a meg.
	 *
	 * OK, so we don't know how big the cache is.  So guess.
	 */
	// batch = 0x2efd6 / 1024 = 0xbb(187) 
	batch = zone->managed_pages / 1024;
	// 0xbb * 0x1000 = 0xbb000(765952) > 512 * 1024 = 524288
	if (batch * PAGE_SIZE > 512 * 1024)
		// batch = 524288 / 0x1000 = 0x80
		batch = (512 * 1024) / PAGE_SIZE;
	// batch = 0x80 / 4 = 0x20(32)
	batch /= 4;		/* We effectively *= 4 below */
	if (batch < 1)
		batch = 1;

	/*
	 * Clamp the batch to a 2^n - 1 value. Having a power
	 * of 2 value was found to be more likely to have
	 * suboptimal cache aliasing properties in some cases.
	 *
	 * For example if 2 tasks are alternately allocating
	 * batches of pages, one task can end up with a lot
	 * of pages of one half of the possible page colors
	 * and the other with pages of the other colors.
	 */
	//rounddown_pow_of_two(0x20 + 0x10 = 0x30) 
	//batch = 31
	batch = rounddown_pow_of_two(batch + batch/2) - 1;

	//return 31;
	return batch;

#else
	/* The deferral and batching of frees should be suppressed under NOMMU
	 * conditions.
	 *
	 * The problem is that NOMMU needs to be able to allocate large chunks
	 * of contiguous memory as there's no hardware page translation to
	 * assemble apparent contiguous memory from discontiguous pages.
	 *
	 * Queueing large contiguous runs of pages for batching, however,
	 * causes the pages to actually be freed in smaller chunks.  As there
	 * can be a significant delay between the individual batches being
	 * recycled, this leads to the once large chunks of space being
	 * fragmented and becoming unavailable for high-order allocations.
	 */
	return 0;
#endif
}

/*
 * pcp->high and pcp->batch values are related and dependent on one another:
 * ->batch must never be higher then ->high.
 * The following function updates them in a safe manner without read side
 * locking.
 *
 * Any new users of pcp->batch and pcp->high should ensure they can cope with
 * those fields changing asynchronously (acording the the above rule).
 *
 * mutex_is_locked(&pcp_batch_high_lock) required when calling this function
 * outside of boot time (or some other assurance that no concurrent updaters
 * exist).
 */
// ARM10C 20140308
// p->pcp: [pcpu0] boot_pageset->pcp, 0, 1
// ARM10C 20150912
// p->pcp: [pcpu0] (&(&contig_page_data)->node_zones[0])->pageset, 186, 31
static void pageset_update(struct per_cpu_pages *pcp, unsigned long high,
		unsigned long batch)
{
       /* start with a fail safe value for batch */
	// pcp->batch: [pcpu0] boot_pageset->pcp->batch
	// pcp->batch: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch
	pcp->batch = 1;
	// pcp->batch: [pcpu0] boot_pageset->pcp->batch: 1
	// pcp->batch: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1

	smp_wmb();

       /* Update high, then batch, in order */
	// pcp->high: [pcpu0] boot_pageset->pcp->high, high: 0
	// pcp->high: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high, high: 186
	pcp->high = high;
	// pcp->high: [pcpu0] boot_pageset->pcp->high: 0
	// pcp->high: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186

	smp_wmb();

	// pcp->batch: [pcpu0] boot_pageset->pcp->batch
	// pcp->batch: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch, batch: 31
	pcp->batch = batch;
	// pcp->batch: [pcpu0] boot_pageset->pcp->batch: 1
	// pcp->batch: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
}

/* a companion to pageset_set_high() */
// ARM10C 20140308
// p: (&boot_pageset + __per_cpu_offset[0]), batch: 0
// ARM10C 20150912
// pcp: [pcp0] (&(&contig_page_data)->node_zones[0])->pageset, 31
static void pageset_set_batch(struct per_cpu_pageset *p, unsigned long batch)
{
	// p->pcp: [pcpu0] boot_pageset->pcp, batch: 0, 1
	// p->pcp: [pcpu0] (&(&contig_page_data)->node_zones[0])->pageset, batch: 31
	pageset_update(&p->pcp, 6 * batch, max(1UL, 1 * batch));

	// pageset_update에서 한일:
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
}

// ARM10C 20140308
// p: (&boot_pageset + __per_cpu_offset[0])
// ARM10C 20150912
// pcp: [pcpu0] (&(&contig_page_data)->node_zones[0])->pageset
static void pageset_init(struct per_cpu_pageset *p)
{
	struct per_cpu_pages *pcp;
	int migratetype;

	// p: (&boot_pageset + __per_cpu_offset[0]), sizeof(*p): 66
	// p: [pcpu0] (&(&contig_page_data)->node_zones[0])->pageset, sizeof(*([pcpu0] (&(&contig_page_data)->node_zones[0])->pageset)): 66
	memset(p, 0, sizeof(*p));

	// p->pcp: [pcpu0] boot_pageset->pcp
	// p->pcp: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->pcp
	pcp = &p->pcp;
	// pcp: [pcpu0] boot_pageset->pcp
	// pcp: [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->pcp

	pcp->count = 0;
	// [pcpu0] boot_pageset->pcp->count: 0
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->count: 0

	// MIGRATE_PCPTYPES: 3
	// MIGRATE_PCPTYPES: 3
	for (migratetype = 0; migratetype < MIGRATE_PCPTYPES; migratetype++)
		// [pcpu0] boot_pageset->pcp->lists[0..2]
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->lists[0..2]
		INIT_LIST_HEAD(&pcp->lists[migratetype]);
}

// ARM10C 20140308
// &per_cpu(boot_pageset, 0): (&boot_pageset + __per_cpu_offset[0]), 0
static void setup_pageset(struct per_cpu_pageset *p, unsigned long batch)
{
	// p: (&boot_pageset + __per_cpu_offset[0])
	pageset_init(p);

	// p: (&boot_pageset + __per_cpu_offset[0]), batch: 0
	pageset_set_batch(p, batch);
}

/*
 * pageset_set_high() sets the high water mark for hot per_cpu_pagelist
 * to the value high for the pageset p.
 */
static void pageset_set_high(struct per_cpu_pageset *p,
				unsigned long high)
{
	unsigned long batch = max(1UL, high / 4);
	if ((high / 4) > (PAGE_SHIFT * 8))
		batch = PAGE_SHIFT * 8;

	pageset_update(&p->pcp, high, batch);
}

// ARM10C 20150912
// zone: &(&contig_page_data)->node_zones[0], pcp: [pcp0] (&(&contig_page_data)->node_zones[0])->pageset
static void __meminit pageset_set_high_and_batch(struct zone *zone,
		struct per_cpu_pageset *pcp)
{
	// percpu_pagelist_fraction: 0
	if (percpu_pagelist_fraction)
		pageset_set_high(pcp,
			(zone->managed_pages /
				percpu_pagelist_fraction));
	else
		// pcp: [pcp0] (&(&contig_page_data)->node_zones[0])->pageset, zone: &(&contig_page_data)->node_zones[0]
		// zone_batchsize(&(&contig_page_data)->node_zones[0]): 31
		pageset_set_batch(pcp, zone_batchsize(zone));

		// pageset_set_batch에서 한일:
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
}

// ARM10C 20150912
// zone: &(&contig_page_data)->node_zones[0], cpu: 0
static void __meminit zone_pageset_init(struct zone *zone, int cpu)
{
	// zone->pageset: (&(&contig_page_data)->node_zones[0])->pageset, cpu: 0
	// per_cpu_ptr((&(&contig_page_data)->node_zones[0])->pageset, 0): [pcp0] (&(&contig_page_data)->node_zones[0])->pageset
	struct per_cpu_pageset *pcp = per_cpu_ptr(zone->pageset, cpu);
	// pcp: [pcp0] (&(&contig_page_data)->node_zones[0])->pageset

	// pcp: [pcp0] (&(&contig_page_data)->node_zones[0])->pageset
	pageset_init(pcp);

	// pageset_init에서 한일:
	// struct per_cpu_pages의 맴버값 초기화 수행
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 0
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 0
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->count: 0
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->lists[0..2] 초기화

	// zone: &(&contig_page_data)->node_zones[0], pcp: [pcp0] (&(&contig_page_data)->node_zones[0])->pageset
	pageset_set_high_and_batch(zone, pcp);

	// pageset_set_high_and_batch에서 한일:
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186
	// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
}

// ARM10C 20150912
// zone: &(&contig_page_data)->node_zones[0]
static void __meminit setup_zone_pageset(struct zone *zone)
{
	int cpu;

	// zone->pageset: (&(&contig_page_data)->node_zones[0])->pageset
	// alloc_percpu(struct per_cpu_pageset): kmem_cache#26-o0 에서 할당된 72 bytes 메모리 주소
	zone->pageset = alloc_percpu(struct per_cpu_pageset);
	// zone->pageset: (&(&contig_page_data)->node_zones[0])->pageset: kmem_cache#26-o0 에서 할당된 72 bytes 메모리 주소

	// nr_cpu_ids: 4, cpumask_next(-1): 0
	for_each_possible_cpu(cpu)
	// for ((cpu) = -1; (cpu) = cpumask_next((cpu), (cpu_possible_mask)), (cpu) < nr_cpu_ids; )
		// zone: &(&contig_page_data)->node_zones[0], cpu: 0
		zone_pageset_init(zone, cpu);

		// zone_pageset_init에서 한일:
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
		// [pcpu0] ((&(&contig_page_data)->node_zones[0])->pageset)->lists[0..2] 초기화
		
		// cpu: 1...3 까지 Loop 수행
}

/*
 * Allocate per cpu pagesets and initialize them.
 * Before this call only boot pagesets were available.
 */
// ARM10C 20150912
void __init setup_per_cpu_pageset(void)
{
	struct zone *zone;

	// first_online_pgdat(): &contig_page_data,
	// zone: &(&contig_page_data)->node_zones[0], populated_zone(&(&contig_page_data)->node_zones[0]): 1
	for_each_populated_zone(zone)
	// for (zone = (first_online_pgdat())->node_zones; zone; zone = next_zone(zone))
	//   if (!populated_zone(zone))
	//      ; /* do nothing */
	//   else
		// zone: &(&contig_page_data)->node_zones[0]
		setup_zone_pageset(zone);

		// setup_zone_pageset에서 한일:
		// (&(&contig_page_data)->node_zones[0])->pageset: kmem_cache#26-o0 에서 할당된 72 bytes 메모리 주소
		// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1
		// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186
		// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
		// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->lists[0..2] 초기화
		
		// zone: ZONE_HIGHMEM: 1, ZONE_MOVABLE: 2 까지 loop 수행

	// 위 loop에서 한일:
	// ZONE_NORMAL: 0 의 수행 결과
	// (&(&contig_page_data)->node_zones[0])->pageset: kmem_cache#26-o0 에서 할당된 72 bytes 메모리 주소
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 1
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->high: 186
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->batch: 31
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[0])->pageset)->lists[0..2] 초기화
	// ZONE_HIGHMEM: 1 의 수행 결과
	// (&(&contig_page_data)->node_zones[1])->pageset: kmem_cache#26-o0 에서 할당된 72 bytes 메모리 주소
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[1])->pageset)->batch: 1
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[1])->pageset)->high: 186
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[1])->pageset)->batch: 31
	// [pcpu0...3] ((&(&contig_page_data)->node_zones[1])->pageset)->lists[0..2] 초기화
	// ZONE_MOVABLE: 2
	// populated_zone(&(&contig_page_data)->node_zones[2]): 0 으로 loop를 빠져나옴
}

// ARM10C 20140111
// zone_size_pages = 0x2F800
// zone_size_pages = 0x50800
static noinline __init_refok
int zone_wait_table_init(struct zone *zone, unsigned long zone_size_pages)
{
	int i;
	struct pglist_data *pgdat = zone->zone_pgdat;
	size_t alloc_size;

	/*
	 * The per-page waitqueue mechanism uses hashed waitqueues
	 * per zone.
	 */
	zone->wait_table_hash_nr_entries =
		// zone_size_pages = 0x2F800
		 wait_table_hash_nr_entries(zone_size_pages);
	//zone->wait_table_hash_nr_entries = 0x400(1024)
	//zone->wait_table_hash_nr_entries = 0x800(2048)

	zone->wait_table_bits =
		//zone->wait_table_hash_nr_entries = 0x400(1024)
		//zone->wait_table_hash_nr_entries = 0x800(2048)
		wait_table_bits(zone->wait_table_hash_nr_entries);
	//zone->wait_table_bits = 10
	//zone->wait_table_bits = 11
	alloc_size = zone->wait_table_hash_nr_entries
					* sizeof(wait_queue_head_t);
	// alloc_size = 1024 * sizeof( wait_queue_head_t ) 
	// alloc_size = 2048 * sizeof( wait_queue_head_t ) 

	if (!slab_is_available()) {	// 수행 
		//zone->wait_table = wait_queue_head_t[1024] 할당 
		//zone->wait_table = wait_queue_head_t[2048] 할당 
		zone->wait_table = (wait_queue_head_t *)
			alloc_bootmem_node_nopanic(pgdat, alloc_size);
	} else {
		/*
		 * This case means that a zone whose size was 0 gets new memory
		 * via memory hot-add.
		 * But it may be the case that a new node was hot-added.  In
		 * this case vmalloc() will not be able to use this new node's
		 * memory - this wait_table must be initialized to use this new
		 * node itself as well.
		 * To use this new node's memory, further consideration will be
		 * necessary.
		 */
		zone->wait_table = vmalloc(alloc_size);
	}
	if (!zone->wait_table)
		return -ENOMEM;

	//zone->wait_table_hash_nr_entries = 1024
	//zone->wait_table_hash_nr_entries = 2048
	for(i = 0; i < zone->wait_table_hash_nr_entries; ++i)
		init_waitqueue_head(zone->wait_table + i);

	return 0;
}

// ARM10C 20140111 
// pcp : per cpu page
// http://www.spinics.net/lists/linux-mm/msg64440.html 상위 주석 참고
static __meminit void zone_pcp_init(struct zone *zone)
{
	/*
	 * per cpu subsystem is not up at this point. The following code
	 * relies on the ability of the linker to provide the
	 * offset of a (static) per cpu variable into the per cpu area.
	 */
	// boot_pageset :  mm/page_alloc.c 에 정의되어 있음
	zone->pageset = &boot_pageset;

	if (populated_zone(zone))
		printk(KERN_DEBUG "  %s zone: %lu pages, LIFO batch:%u\n",
			zone->name, zone->present_pages,
					//zone_batchsize(zone) : 31
					 zone_batchsize(zone));
}

// ARM10C 20140111 
//zone_start_pfn = 0x20000, size = 0x2f800, context = 0
//zone_start_pfn = 0x4F800, size = 0x50800, MEMMAP_EARLY = 0
int __meminit init_currently_empty_zone(struct zone *zone,
					unsigned long zone_start_pfn,
					unsigned long size,
					enum memmap_context context)
{
	// pgdat = (&contig_page_data) 
	struct pglist_data *pgdat = zone->zone_pgdat;
	int ret;
	// size = 0x2F800
	// size = 0x50800
	// 1024개의 wait_table(hash)을 할당, 초기화
	// 2048개의 wait_table(hash)을 할당, 초기화
	ret = zone_wait_table_init(zone, size);
	if (ret)
		return ret;
	//pgdat->nr_zones = 0 + 1 = 1
	//pgdat->nr_zones = 1 + 1 = 2
	pgdat->nr_zones = zone_idx(zone) + 1;

	//zone->zone_start_pfn = 0x20000;
	//zone->zone_start_pfn = 0x4F800;
	zone->zone_start_pfn = zone_start_pfn;

	mminit_dprintk(MMINIT_TRACE, "memmap_init",
			"Initialising map node %d zone %lu pfns %lu -> %lu\n",
			pgdat->node_id,
			(unsigned long)zone_idx(zone),
			zone_start_pfn, (zone_start_pfn + size));

	//zone->free_area[].free_list[] 리스트초기화
	//zone->free_area[].nr_free = 0
	zone_init_free_lists(zone);

	return 0;
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP	// ARM10C CONFIG_HAVE_MEMBLOCK_NODE_MAP = n 
#ifndef CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID
/*
 * Required by SPARSEMEM. Given a PFN, return what node the PFN is on.
 * Architectures may implement their own version but if add_active_range()
 * was used and there are no special requirements, this is a convenient
 * alternative
 */
int __meminit __early_pfn_to_nid(unsigned long pfn)
{
	unsigned long start_pfn, end_pfn;
	int nid;
	/*
	 * NOTE: The following SMP-unsafe globals are only used early in boot
	 * when the kernel is running single-threaded.
	 */
	static unsigned long __meminitdata last_start_pfn, last_end_pfn;
	static int __meminitdata last_nid;

	if (last_start_pfn <= pfn && pfn < last_end_pfn)
		return last_nid;

	nid = memblock_search_pfn_nid(pfn, &start_pfn, &end_pfn);
	if (nid != -1) {
		last_start_pfn = start_pfn;
		last_end_pfn = end_pfn;
		last_nid = nid;
	}

	return nid;
}
#endif /* CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID */

int __meminit early_pfn_to_nid(unsigned long pfn)
{
	int nid;

	nid = __early_pfn_to_nid(pfn);
	if (nid >= 0)
		return nid;
	/* just returns 0 */
	return 0;
}

#ifdef CONFIG_NODES_SPAN_OTHER_NODES
bool __meminit early_pfn_in_nid(unsigned long pfn, int node)
{
	int nid;

	nid = __early_pfn_to_nid(pfn);
	if (nid >= 0 && nid != node)
		return false;
	return true;
}
#endif

/**
 * free_bootmem_with_active_regions - Call free_bootmem_node for each active range
 * @nid: The node to free memory on. If MAX_NUMNODES, all nodes are freed.
 * @max_low_pfn: The highest PFN that will be passed to free_bootmem_node
 *
 * If an architecture guarantees that all ranges registered with
 * add_active_ranges() contain no holes and may be freed, this
 * this function may be used instead of calling free_bootmem() manually.
 */
void __init free_bootmem_with_active_regions(int nid, unsigned long max_low_pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, this_nid;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, &this_nid) {
		start_pfn = min(start_pfn, max_low_pfn);
		end_pfn = min(end_pfn, max_low_pfn);

		if (start_pfn < end_pfn)
			free_bootmem_node(NODE_DATA(this_nid),
					  PFN_PHYS(start_pfn),
					  (end_pfn - start_pfn) << PAGE_SHIFT);
	}
}

/**
 * sparse_memory_present_with_active_regions - Call memory_present for each active range
 * @nid: The node to call memory_present for. If MAX_NUMNODES, all nodes will be used.
 *
 * If an architecture guarantees that all ranges registered with
 * add_active_ranges() contain no holes and may be freed, this
 * function may be used instead of calling memory_present() manually.
 */
void __init sparse_memory_present_with_active_regions(int nid)
{
	unsigned long start_pfn, end_pfn;
	int i, this_nid;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, &this_nid)
		memory_present(this_nid, start_pfn, end_pfn);
}

/**
 * get_pfn_range_for_nid - Return the start and end page frames for a node
 * @nid: The nid to return the range for. If MAX_NUMNODES, the min and max PFN are returned.
 * @start_pfn: Passed by reference. On return, it will have the node start_pfn.
 * @end_pfn: Passed by reference. On return, it will have the node end_pfn.
 *
 * It returns the start and end page frame of a node based on information
 * provided by an arch calling add_active_range(). If called for a node
 * with no available memory, a warning is printed and the start and end
 * PFNs will be 0.
 */
void __meminit get_pfn_range_for_nid(unsigned int nid,
			unsigned long *start_pfn, unsigned long *end_pfn)
{
	unsigned long this_start_pfn, this_end_pfn;
	int i;

	*start_pfn = -1UL;
	*end_pfn = 0;

	for_each_mem_pfn_range(i, nid, &this_start_pfn, &this_end_pfn, NULL) {
		*start_pfn = min(*start_pfn, this_start_pfn);
		*end_pfn = max(*end_pfn, this_end_pfn);
	}

	if (*start_pfn == -1UL)
		*start_pfn = 0;
}

/*
 * This finds a zone that can be used for ZONE_MOVABLE pages. The
 * assumption is made that zones within a node are ordered in monotonic
 * increasing memory addresses so that the "highest" populated zone is used
 */
static void __init find_usable_zone_for_movable(void)
{
	int zone_index;
	for (zone_index = MAX_NR_ZONES - 1; zone_index >= 0; zone_index--) {
		if (zone_index == ZONE_MOVABLE)
			continue;

		if (arch_zone_highest_possible_pfn[zone_index] >
				arch_zone_lowest_possible_pfn[zone_index])
			break;
	}

	VM_BUG_ON(zone_index == -1);
	movable_zone = zone_index;
}

/*
 * The zone ranges provided by the architecture do not include ZONE_MOVABLE
 * because it is sized independent of architecture. Unlike the other zones,
 * the starting point for ZONE_MOVABLE is not fixed. It may be different
 * in each node depending on the size of each node and how evenly kernelcore
 * is distributed. This helper function adjusts the zone ranges
 * provided by the architecture for a given node by using the end of the
 * highest usable zone for ZONE_MOVABLE. This preserves the assumption that
 * zones within a node are in order of monotonic increases memory addresses
 */
static void __meminit adjust_zone_range_for_zone_movable(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	/* Only adjust if ZONE_MOVABLE is on this node */
	if (zone_movable_pfn[nid]) {
		/* Size ZONE_MOVABLE */
		if (zone_type == ZONE_MOVABLE) {
			*zone_start_pfn = zone_movable_pfn[nid];
			*zone_end_pfn = min(node_end_pfn,
				arch_zone_highest_possible_pfn[movable_zone]);

		/* Adjust for ZONE_MOVABLE starting within this range */
		} else if (*zone_start_pfn < zone_movable_pfn[nid] &&
				*zone_end_pfn > zone_movable_pfn[nid]) {
			*zone_end_pfn = zone_movable_pfn[nid];

		/* Check if this whole range is within ZONE_MOVABLE */
		} else if (*zone_start_pfn >= zone_movable_pfn[nid])
			*zone_start_pfn = *zone_end_pfn;
	}
}

/*
 * Return the number of pages a zone spans in a node, including holes
 * present_pages = zone_spanned_pages_in_node() - zone_absent_pages_in_node()
 */
static unsigned long __meminit zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *ignored)
{
	unsigned long zone_start_pfn, zone_end_pfn;

	/* Get the start and end of the zone */
	zone_start_pfn = arch_zone_lowest_possible_pfn[zone_type];
	zone_end_pfn = arch_zone_highest_possible_pfn[zone_type];
	adjust_zone_range_for_zone_movable(nid, zone_type,
				node_start_pfn, node_end_pfn,
				&zone_start_pfn, &zone_end_pfn);

	/* Check that this node has pages within the zone's required range */
	if (zone_end_pfn < node_start_pfn || zone_start_pfn > node_end_pfn)
		return 0;

	/* Move the zone boundaries inside the node if necessary */
	zone_end_pfn = min(zone_end_pfn, node_end_pfn);
	zone_start_pfn = max(zone_start_pfn, node_start_pfn);

	/* Return the spanned pages */
	return zone_end_pfn - zone_start_pfn;
}

/*
 * Return the number of holes in a range on a node. If nid is MAX_NUMNODES,
 * then all holes in the requested range will be accounted for.
 */
unsigned long __meminit __absent_pages_in_range(int nid,
				unsigned long range_start_pfn,
				unsigned long range_end_pfn)
{
	unsigned long nr_absent = range_end_pfn - range_start_pfn;
	unsigned long start_pfn, end_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
		start_pfn = clamp(start_pfn, range_start_pfn, range_end_pfn);
		end_pfn = clamp(end_pfn, range_start_pfn, range_end_pfn);
		nr_absent -= end_pfn - start_pfn;
	}
	return nr_absent;
}

/**
 * absent_pages_in_range - Return number of page frames in holes within a range
 * @start_pfn: The start PFN to start searching for holes
 * @end_pfn: The end PFN to stop searching for holes
 *
 * It returns the number of pages frames in memory holes within a range.
 */
unsigned long __init absent_pages_in_range(unsigned long start_pfn,
							unsigned long end_pfn)
{
	return __absent_pages_in_range(MAX_NUMNODES, start_pfn, end_pfn);
}

/* Return the number of page frames in holes in a zone on a node */
static unsigned long __meminit zone_absent_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *ignored)
{
	unsigned long zone_low = arch_zone_lowest_possible_pfn[zone_type];
	unsigned long zone_high = arch_zone_highest_possible_pfn[zone_type];
	unsigned long zone_start_pfn, zone_end_pfn;

	zone_start_pfn = clamp(node_start_pfn, zone_low, zone_high);
	zone_end_pfn = clamp(node_end_pfn, zone_low, zone_high);

	adjust_zone_range_for_zone_movable(nid, zone_type,
			node_start_pfn, node_end_pfn,
			&zone_start_pfn, &zone_end_pfn);
	return __absent_pages_in_range(nid, zone_start_pfn, zone_end_pfn);
}

#else /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

// ARM10C 20140111 
static inline unsigned long __meminit zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *zones_size)
{
	return zones_size[zone_type];
}

// ARM10C 20140111
static inline unsigned long __meminit zone_absent_pages_in_node(int nid,
						unsigned long zone_type,
						unsigned long node_start_pfn,
						unsigned long node_end_pfn,
						unsigned long *zholes_size)
{
	if (!zholes_size)
		return 0;

	return zholes_size[zone_type];
}

#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

// ARM10C 20140111 
//start_pfn = 0, end_pfn = 0
static void __meminit calculate_node_totalpages(struct pglist_data *pgdat,
						unsigned long node_start_pfn,
						unsigned long node_end_pfn,
						unsigned long *zones_size,
						unsigned long *zholes_size)
{
	unsigned long realtotalpages, totalpages = 0;
	enum zone_type i;

	//MAX_NR_ZONES = 3
	for (i = 0; i < MAX_NR_ZONES; i++)
		//totalpages = 0x2f800(zone_size[0]) + 0x50800(zone_size[1]) = 0x80000(2GByte) 
		totalpages += zone_spanned_pages_in_node(pgdat->node_id, i,
							 node_start_pfn,
							 node_end_pfn,
							 zones_size);
	
	//pgdat->node_spanned_pages = 0x80000;
	pgdat->node_spanned_pages = totalpages;

	//realtotalpages = 0x80000;
	realtotalpages = totalpages;
	//MAX_NR_ZONES = 3
	for (i = 0; i < MAX_NR_ZONES; i++)
		//realtatalpages = 0x80000 - 0x0(zhone_size[0]) - 0x0(zhole_size[1]) = 0x80000
		realtotalpages -=
			zone_absent_pages_in_node(pgdat->node_id, i,
						  node_start_pfn, node_end_pfn,
						  zholes_size);
	//pgdat->node_present_pages = 0x80000;
	pgdat->node_present_pages = realtotalpages;
	printk(KERN_DEBUG "On node %d totalpages: %lu\n", pgdat->node_id,
							realtotalpages);
}

#ifndef CONFIG_SPARSEMEM	// CONFIG_SPARSEMEM = y 
/*
 * Calculate the size of the zone->blockflags rounded to an unsigned long
 * Start by making sure zonesize is a multiple of pageblock_order by rounding
 * up. Then use 1 NR_PAGEBLOCK_BITS worth of bits per pageblock, finally
 * round what is now in bits to nearest long in bits, then return it in
 * bytes.
 */
static unsigned long __init usemap_size(unsigned long zone_start_pfn, unsigned long zonesize)
{
	unsigned long usemapsize;

	zonesize += zone_start_pfn & (pageblock_nr_pages-1);
	usemapsize = roundup(zonesize, pageblock_nr_pages);
	usemapsize = usemapsize >> pageblock_order;
	usemapsize *= NR_PAGEBLOCK_BITS;
	usemapsize = roundup(usemapsize, 8 * sizeof(unsigned long));

	return usemapsize / 8;
}

static void __init setup_usemap(struct pglist_data *pgdat,
				struct zone *zone,
				unsigned long zone_start_pfn,
				unsigned long zonesize)
{
	unsigned long usemapsize = usemap_size(zone_start_pfn, zonesize);
	zone->pageblock_flags = NULL;
	if (usemapsize)
		zone->pageblock_flags = alloc_bootmem_node_nopanic(pgdat,
								   usemapsize);
}
#else
// ARM10C 20140111 
static inline void setup_usemap(struct pglist_data *pgdat, struct zone *zone,
				unsigned long zone_start_pfn, unsigned long zonesize) {}
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE // CONFIG_HUGETLB_PAGE_SIZE_VARIABLE=n

/* Initialise the number of pages represented by NR_PAGEBLOCK_BITS */
void __paginginit set_pageblock_order(void)
{
	unsigned int order;

	/* Check that pageblock_nr_pages has not already been setup */
	if (pageblock_order)
		return;

	if (HPAGE_SHIFT > PAGE_SHIFT)
		order = HUGETLB_PAGE_ORDER;
	else
		order = MAX_ORDER - 1;

	/*
	 * Assume the largest contiguous order of interest is a huge page.
	 * This value may be variable depending on boot parameters on IA64 and
	 * powerpc.
	 */
	pageblock_order = order;
}
#else /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * When CONFIG_HUGETLB_PAGE_SIZE_VARIABLE is not set, set_pageblock_order()
 * is unused as pageblock_order is set at compile-time. See
 * include/linux/pageblock-flags.h for the values of pageblock_order based on
 * the kernel config
 */
// ARM10C 20131214
// ARM10C 20140111 
void __paginginit set_pageblock_order(void)
{
}

#endif /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

// ARM10C 20140111 
// spanned_pages = 0x2f800, present_pages = 0x2f800
static unsigned long __paginginit calc_memmap_size(unsigned long spanned_pages,
						   unsigned long present_pages)
{
	// pages = 0x2f800
	unsigned long pages = spanned_pages;

	/*
	 * Provide a more accurate estimation if there are holes within
	 * the zone and SPARSEMEM is in use. If there are holes within the
	 * zone, each populated memory region may cost us one or two extra
	 * memmap pages due to alignment because memmap pages for each
	 * populated regions may not naturally algined on page boundary.
	 * So the (present_pages >> 4) heuristic is a tradeoff for that.
	 */
	if (spanned_pages > present_pages + (present_pages >> 4) &&
	    IS_ENABLED(CONFIG_SPARSEMEM))
		pages = present_pages;

	//총 할당한 page 갯수를 리턴
	//PAGE_ALIGN( 0x2f800 * 44 ) >> PAGE_SHIFT
	//0x82a000 >> PAGE_SHIFT = 0x82a;
	return PAGE_ALIGN(pages * sizeof(struct page)) >> PAGE_SHIFT;
}

/*
 * Set up the zone data structures:
 *   - mark all pages reserved
 *   - mark all memory queues empty
 *   - clear the memory bitmaps
 *
 * NOTE: pgdat should get zeroed by caller.
 */
// ARM10C 20140111
// node_start_pfn = 0, node_end_pfn = 0
static void __paginginit free_area_init_core(struct pglist_data *pgdat,
		unsigned long node_start_pfn, unsigned long node_end_pfn,
		unsigned long *zones_size, unsigned long *zholes_size)
{
	enum zone_type j;
	int nid = pgdat->node_id;//0
	//zone_start_pfn = 0x20000;
	unsigned long zone_start_pfn = pgdat->node_start_pfn;
	int ret;

	pgdat_resize_init(pgdat);//empty function
#ifdef CONFIG_NUMA_BALANCING // CONFIG_NUMA_BALANCING = n
	spin_lock_init(&pgdat->numabalancing_migrate_lock);
	pgdat->numabalancing_migrate_nr_pages = 0;
	pgdat->numabalancing_migrate_next_window = jiffies;
#endif
	init_waitqueue_head(&pgdat->kswapd_wait); //kswapd_wait 자료구조 초기화
	init_waitqueue_head(&pgdat->pfmemalloc_wait); //pfmemalloc_wait 자료구조 초기화
	pgdat_page_cgroup_init(pgdat);//empty function

	// MAX_NR_ZONES : 3
	for (j = 0; j < MAX_NR_ZONES; j++) {
		//zone = &pgdat->node_zones[ZONE_NORMAL]
		//zone = &pgdat->node_zones[ZONE_HIGHMEM]
		//zone = &pgdat->node_zones[ZONE_MOVABLE]
		struct zone *zone = pgdat->node_zones + j;
		unsigned long size, realsize, freesize, memmap_pages;

		// size = 0x2f800
		// size = 0x50800
		// size = 0
		size = zone_spanned_pages_in_node(nid, j, node_start_pfn,
						  node_end_pfn, zones_size);
		// realsize = freesize = 0x2f800 - 0x0 = 0x2f800
		// realsize = freesize = 0x50800 - 0x0 = 0x50800
		// realsize = freesize = 0x0 - 0x0 = 0x0
		realsize = freesize = size - zone_absent_pages_in_node(nid, j,
								node_start_pfn,
								node_end_pfn,
								zholes_size);

		/*
		 * Adjust freesize so that it accounts for how much memory
		 * is used by this zone for memmap. This affects the watermark
		 * and per-cpu initialisations
		 */
		// size = 0x2F800, realsize = 0x2f800
		// size = 0x50800, realsize = 0x50800
		// size = 0x0, realsize = 0x0
		memmap_pages = calc_memmap_size(size, realsize);
		//memmap_pages = 0x82a(2090)
		//memmap_pages = 0xDD6(3542)
		//memmap_pages = 0x0(0)
		if (freesize >= memmap_pages) {
			//freesize = 0x2f800 - 0x82a = 0x2efd6
			//freesize = 0x50800 - 0xDD6 = 0x4FA2A
			//freesize = 0x0 - 0x0 = 0x0
			freesize -= memmap_pages;
			if (memmap_pages)
				printk(KERN_DEBUG
				       "  %s zone: %lu pages used for memmap\n",
				       zone_names[j], memmap_pages);
		} else
			printk(KERN_WARNING
				"  %s zone: %lu pages exceeds freesize %lu\n",
				zone_names[j], memmap_pages, freesize);

		/* Account for reserved pages */
		// j = 0, freesize = 0x2efd6, dma_reserve = 0
		// j = 1, freesize = 0x4FA2A, dma_reserve = 0
		// j = 2, freesize = 0x0, dma_reserve = 0
		if (j == 0 && freesize > dma_reserve) {
			// j = 0, freesize = 0x2efd6 - 0 = 0x2efd6
			freesize -= dma_reserve;
			printk(KERN_DEBUG "  %s zone: %lu pages reserved\n",
					zone_names[0], dma_reserve);
		}

		if (!is_highmem_idx(j))
			// j = 0, nr_kernel_pages = 0 + 0x2efd6
			nr_kernel_pages += freesize;
		/* Charge for highmem memmap if there are enough kernel pages */
		// nr_kernel_pages : 0x2EFD6, memmap_pages * 2 : 0x1BAC
		else if (nr_kernel_pages > memmap_pages * 2)
			// nr_kernel_pages : 0x2E200
			nr_kernel_pages -= memmap_pages;

		//j = 0, nr_all_pages = 0 + 0x2efd6
		//j = 1, nr_all_pages = 0x2efd6 + 0x4FA2A
		//j = 2, nr_all_pages = 0x7EA00 + 0
		nr_all_pages += freesize;

		//j = 0, zone->spanned_pages = 0x2f800
		//j = 1, zone->spanned_pages = 0x50800
		//j = 2, zone->spanned_pages = 0x0
		zone->spanned_pages = size;
		//j = 0, zone->present_pages = 0x2f800
		//j = 1, zone->present_pages = 0x50800
		//j = 2, zone->present_pages = 0x0
		zone->present_pages = realsize;
		/*
		 * Set an approximate value for lowmem here, it will be adjusted
		 * when the bootmem allocator frees pages into the buddy system.
		 * And all highmem pages will be managed by the buddy system.
		 */
		//j = 0, zone->managed_pages = 0x2efd6
		//j = 1, zone->managed_pages = 0x50800
		//j = 2, zone->managed_pages = 0x0
		zone->managed_pages = is_highmem_idx(j) ? realsize : freesize;
#ifdef CONFIG_NUMA // CONFIG_NUMA = n
		zone->node = nid;
		zone->min_unmapped_pages = (freesize*sysctl_min_unmapped_ratio)
						/ 100;
		zone->min_slab_pages = (freesize * sysctl_min_slab_ratio) / 100;
#endif
		//j = 0, zone->name = "Normal"
		//j = 1, zone->name = "HighMem"
		//j = 2, zone->name = "Movable"
		zone->name = zone_names[j];
		spin_lock_init(&zone->lock);
		spin_lock_init(&zone->lru_lock);
		zone_seqlock_init(zone); //empty function
		//j = 0, zone->zone_pgdat = (&contig_page_data)
		//j = 1, zone->zone_pgdat = (&contig_page_data)
		//j = 2, zone->zone_pgdat = (&contig_page_data)
		zone->zone_pgdat = pgdat;
		// j = 0, zone->pageset = &boot_pageset
		// j = 1, zone->pageset = &boot_pageset
		// j = 2, zone->pageset = &boot_pageset
		zone_pcp_init(zone);

		/* For bootup, initialized properly in watermark setup */
		// ARM10C 20140510
		//j = 0, zone = &pgdat->node_zones[ZONE_NORMAL], zone->managed_pages = 0x2efd6
		//j = 1, zone = &pgdat->node_zones[ZONE_HIGHMEM], zone->managed_pages = 0x50800
		//j = 2, zone = &pgdat->node_zones[ZONE_MOVABLE], zone->managed_pages = 0x0
		mod_zone_page_state(zone, NR_ALLOC_BATCH, zone->managed_pages);

		// least recently used vector init
		lruvec_init(&zone->lruvec);
		//j = 0, size = 0x2f800
		//j = 1, size = 0x50800
		//j = 2, size = 0x0

		if (!size)
			continue;

		set_pageblock_order(); // empty function
		//j = 0, zone_start_pfn = 0x20000, size = 0x2f800
		//j = 1, zone_start_pfn = 0x4F800, size = 0x50800
		setup_usemap(pgdat, zone, zone_start_pfn, size); // empty function
		//j = 0, zone_start_pfn = 0x20000, size = 0x2f800, MEMMAP_EARLY = 0
		//j = 1, zone_start_pfn = 0x4F800, size = 0x50800, MEMMAP_EARLY = 0
		ret = init_currently_empty_zone(zone, zone_start_pfn,
						size, MEMMAP_EARLY);
		//zone wait_table, free_area관련 멤버를 초기화

// 2014/01/11 종료
// 2014/01/18 시작

		BUG_ON(ret);
		// j = 0, size : 0x2F800, nid : 0, j : 0, zone_start_pfn : 0x20000
		// j = 1, size : 0x50800, nid : 0, j : 1, zone_start_pfn : 0x4F800
		memmap_init(size, nid, j, zone_start_pfn);
		// struct page 내부 멤버를 설정
		// flags : section, node, zone 번호, PG_reserved(10) 설정
		// page->__count.counter : 1
		// page->_mapcount.counter : -1
		// page->lru 초기화
		// page에 해당하는 &mem_section[0][2]->pageblock_flags의
		// MIGRATE_MOVABLE(2)번 비트를 1로 설정(pageblock마다)
		// mem_section[0][2] ~ mem_section[0][9] 까지 설정

		// zone_start_pfn : 0x4F800
		// zone_start_pfn : 0xA0000
		zone_start_pfn += size;
	}
}

// ARM10C 20140111
// ARM10C 20140329
static void __init_refok alloc_node_mem_map(struct pglist_data *pgdat)
{
	/* Skip empty nodes */
	if (!pgdat->node_spanned_pages)
		return;

	// ARM10C 20140329
	// mem_map은NUMA에서 사용하는 것으로 보임
#ifdef CONFIG_FLAT_NODE_MEM_MAP // CONFIG_FLAG_NODE_MEM_MAP = n
	/* ia64 gets its own node_mem_map, before this, without bootmem */
	if (!pgdat->node_mem_map) {
		unsigned long size, start, end;
		struct page *map;

		/*
		 * The zone's endpoints aren't required to be MAX_ORDER
		 * aligned but the node_mem_map endpoints must be in order
		 * for the buddy allocator to function correctly.
		 */
		start = pgdat->node_start_pfn & ~(MAX_ORDER_NR_PAGES - 1);
		end = pgdat_end_pfn(pgdat);
		end = ALIGN(end, MAX_ORDER_NR_PAGES);
		size =  (end - start) * sizeof(struct page);
		map = alloc_remap(pgdat->node_id, size);
		if (!map)
			map = alloc_bootmem_node_nopanic(pgdat, size);
		pgdat->node_mem_map = map + (pgdat->node_start_pfn - start);
	}
#ifndef CONFIG_NEED_MULTIPLE_NODES
	/*
	 * With no DISCONTIG, the global mem_map is just set as node 0's
	 */
	if (pgdat == NODE_DATA(0)) {
		mem_map = NODE_DATA(0)->node_mem_map;
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
		if (page_to_pfn(mem_map) != pgdat->node_start_pfn)
			mem_map -= (pgdat->node_start_pfn - ARCH_PFN_OFFSET);
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */
	}
#endif
#endif /* CONFIG_FLAT_NODE_MEM_MAP */// CONFIG_FLAG_NODE_MEM_MAP = n
}

// ARM10C 20140111
// nid = 0, node_start_pfn = 0x20000
void __paginginit free_area_init_node(int nid, unsigned long *zones_size,
		unsigned long node_start_pfn, unsigned long *zholes_size)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	unsigned long start_pfn = 0;
	unsigned long end_pfn = 0;

	/* pg_data_t should be reset to zero when it's allocated */
	WARN_ON(pgdat->nr_zones || pgdat->classzone_idx);

	//pgdat->node_id = 0;
	pgdat->node_id = nid;
	//pgdat->node_start_pfn = 0x20000;
	pgdat->node_start_pfn = node_start_pfn;
	init_zone_allows_reclaim(nid);	// Empty function
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP	// ARM10C  CONFIG_HAVE_MEMBLOCK_NODE_MAP = n
	get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);
#endif
	//start_pfn = 0, end_pfn = 0
	//pgdat 구조체
	//pgdat->node_spanned_pages = 0x80000;
	//pgdat->node_present_pages = 0x80000;
	calculate_node_totalpages(pgdat, start_pfn, end_pfn,
				  zones_size, zholes_size);

	alloc_node_mem_map(pgdat);
#ifdef CONFIG_FLAT_NODE_MEM_MAP	//CONFIG_FLAT_NODE_MEM_MAP = n
	printk(KERN_DEBUG "free_area_init_node: node %d, pgdat %08lx, node_mem_map %08lx\n",
		nid, (unsigned long)pgdat,
		(unsigned long)pgdat->node_mem_map);
#endif

	//start_pfn = 0, end_pfn = 0
	free_area_init_core(pgdat, start_pfn, end_pfn,
			    zones_size, zholes_size);
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP

#if MAX_NUMNODES > 1
/*
 * Figure out the number of possible node ids.
 */
void __init setup_nr_node_ids(void)
{
	unsigned int node;
	unsigned int highest = 0;

	for_each_node_mask(node, node_possible_map)
		highest = node;
	nr_node_ids = highest + 1;
}
#endif

/**
 * node_map_pfn_alignment - determine the maximum internode alignment
 *
 * This function should be called after node map is populated and sorted.
 * It calculates the maximum power of two alignment which can distinguish
 * all the nodes.
 *
 * For example, if all nodes are 1GiB and aligned to 1GiB, the return value
 * would indicate 1GiB alignment with (1 << (30 - PAGE_SHIFT)).  If the
 * nodes are shifted by 256MiB, 256MiB.  Note that if only the last node is
 * shifted, 1GiB is enough and this function will indicate so.
 *
 * This is used to test whether pfn -> nid mapping of the chosen memory
 * model has fine enough granularity to avoid incorrect mapping for the
 * populated node map.
 *
 * Returns the determined alignment in pfn's.  0 if there is no alignment
 * requirement (single node).
 */
unsigned long __init node_map_pfn_alignment(void)
{
	unsigned long accl_mask = 0, last_end = 0;
	unsigned long start, end, mask;
	int last_nid = -1;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		if (!start || last_nid < 0 || last_nid == nid) {
			last_nid = nid;
			last_end = end;
			continue;
		}

		/*
		 * Start with a mask granular enough to pin-point to the
		 * start pfn and tick off bits one-by-one until it becomes
		 * too coarse to separate the current node from the last.
		 */
		mask = ~((1 << __ffs(start)) - 1);
		while (mask && last_end <= (start & (mask << 1)))
			mask <<= 1;

		/* accumulate all internode masks */
		accl_mask |= mask;
	}

	/* convert mask to number of pages */
	return ~accl_mask + 1;
}

/* Find the lowest pfn for a node */
static unsigned long __init find_min_pfn_for_node(int nid)
{
	unsigned long min_pfn = ULONG_MAX;
	unsigned long start_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, NULL, NULL)
		min_pfn = min(min_pfn, start_pfn);

	if (min_pfn == ULONG_MAX) {
		printk(KERN_WARNING
			"Could not find start_pfn for node %d\n", nid);
		return 0;
	}

	return min_pfn;
}

/**
 * find_min_pfn_with_active_regions - Find the minimum PFN registered
 *
 * It returns the minimum PFN based on information provided via
 * add_active_range().
 */
unsigned long __init find_min_pfn_with_active_regions(void)
{
	return find_min_pfn_for_node(MAX_NUMNODES);
}

/*
 * early_calculate_totalpages()
 * Sum pages in active regions for movable zone.
 * Populate N_MEMORY for calculating usable_nodes.
 */
static unsigned long __init early_calculate_totalpages(void)
{
	unsigned long totalpages = 0;
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		unsigned long pages = end_pfn - start_pfn;

		totalpages += pages;
		if (pages)
			node_set_state(nid, N_MEMORY);
	}
	return totalpages;
}

/*
 * Find the PFN the Movable zone begins in each node. Kernel memory
 * is spread evenly between nodes as long as the nodes have enough
 * memory. When they don't, some nodes will have more kernelcore than
 * others
 */
static void __init find_zone_movable_pfns_for_nodes(void)
{
	int i, nid;
	unsigned long usable_startpfn;
	unsigned long kernelcore_node, kernelcore_remaining;
	/* save the state before borrow the nodemask */
	nodemask_t saved_node_state = node_states[N_MEMORY];
	unsigned long totalpages = early_calculate_totalpages();
	int usable_nodes = nodes_weight(node_states[N_MEMORY]);

	/*
	 * If movablecore was specified, calculate what size of
	 * kernelcore that corresponds so that memory usable for
	 * any allocation type is evenly spread. If both kernelcore
	 * and movablecore are specified, then the value of kernelcore
	 * will be used for required_kernelcore if it's greater than
	 * what movablecore would have allowed.
	 */
	if (required_movablecore) {
		unsigned long corepages;

		/*
		 * Round-up so that ZONE_MOVABLE is at least as large as what
		 * was requested by the user
		 */
		required_movablecore =
			roundup(required_movablecore, MAX_ORDER_NR_PAGES);
		corepages = totalpages - required_movablecore;

		required_kernelcore = max(required_kernelcore, corepages);
	}

	/* If kernelcore was not specified, there is no ZONE_MOVABLE */
	if (!required_kernelcore)
		goto out;

	/* usable_startpfn is the lowest possible pfn ZONE_MOVABLE can be at */
	find_usable_zone_for_movable();
	usable_startpfn = arch_zone_lowest_possible_pfn[movable_zone];

restart:
	/* Spread kernelcore memory as evenly as possible throughout nodes */
	kernelcore_node = required_kernelcore / usable_nodes;
	for_each_node_state(nid, N_MEMORY) {
		unsigned long start_pfn, end_pfn;

		/*
		 * Recalculate kernelcore_node if the division per node
		 * now exceeds what is necessary to satisfy the requested
		 * amount of memory for the kernel
		 */
		if (required_kernelcore < kernelcore_node)
			kernelcore_node = required_kernelcore / usable_nodes;

		/*
		 * As the map is walked, we track how much memory is usable
		 * by the kernel using kernelcore_remaining. When it is
		 * 0, the rest of the node is usable by ZONE_MOVABLE
		 */
		kernelcore_remaining = kernelcore_node;

		/* Go through each range of PFNs within this node */
		for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
			unsigned long size_pages;

			start_pfn = max(start_pfn, zone_movable_pfn[nid]);
			if (start_pfn >= end_pfn)
				continue;

			/* Account for what is only usable for kernelcore */
			if (start_pfn < usable_startpfn) {
				unsigned long kernel_pages;
				kernel_pages = min(end_pfn, usable_startpfn)
								- start_pfn;

				kernelcore_remaining -= min(kernel_pages,
							kernelcore_remaining);
				required_kernelcore -= min(kernel_pages,
							required_kernelcore);

				/* Continue if range is now fully accounted */
				if (end_pfn <= usable_startpfn) {

					/*
					 * Push zone_movable_pfn to the end so
					 * that if we have to rebalance
					 * kernelcore across nodes, we will
					 * not double account here
					 */
					zone_movable_pfn[nid] = end_pfn;
					continue;
				}
				start_pfn = usable_startpfn;
			}

			/*
			 * The usable PFN range for ZONE_MOVABLE is from
			 * start_pfn->end_pfn. Calculate size_pages as the
			 * number of pages used as kernelcore
			 */
			size_pages = end_pfn - start_pfn;
			if (size_pages > kernelcore_remaining)
				size_pages = kernelcore_remaining;
			zone_movable_pfn[nid] = start_pfn + size_pages;

			/*
			 * Some kernelcore has been met, update counts and
			 * break if the kernelcore for this node has been
			 * satisfied
			 */
			required_kernelcore -= min(required_kernelcore,
								size_pages);
			kernelcore_remaining -= size_pages;
			if (!kernelcore_remaining)
				break;
		}
	}

	/*
	 * If there is still required_kernelcore, we do another pass with one
	 * less node in the count. This will push zone_movable_pfn[nid] further
	 * along on the nodes that still have memory until kernelcore is
	 * satisfied
	 */
	usable_nodes--;
	if (usable_nodes && required_kernelcore > usable_nodes)
		goto restart;

	/* Align start of ZONE_MOVABLE on all nids to MAX_ORDER_NR_PAGES */
	for (nid = 0; nid < MAX_NUMNODES; nid++)
		zone_movable_pfn[nid] =
			roundup(zone_movable_pfn[nid], MAX_ORDER_NR_PAGES);

out:
	/* restore the node_state */
	node_states[N_MEMORY] = saved_node_state;
}

/* Any regular or high memory on that node ? */
static void check_for_memory(pg_data_t *pgdat, int nid)
{
	enum zone_type zone_type;

	if (N_MEMORY == N_NORMAL_MEMORY)
		return;

	for (zone_type = 0; zone_type <= ZONE_MOVABLE - 1; zone_type++) {
		struct zone *zone = &pgdat->node_zones[zone_type];
		if (populated_zone(zone)) {
			node_set_state(nid, N_HIGH_MEMORY);
			if (N_NORMAL_MEMORY != N_HIGH_MEMORY &&
			    zone_type <= ZONE_NORMAL)
				node_set_state(nid, N_NORMAL_MEMORY);
			break;
		}
	}
}

/**
 * free_area_init_nodes - Initialise all pg_data_t and zone data
 * @max_zone_pfn: an array of max PFNs for each zone
 *
 * This will call free_area_init_node() for each active node in the system.
 * Using the page ranges provided by add_active_range(), the size of each
 * zone in each node and their holes is calculated. If the maximum PFN
 * between two adjacent zones match, it is assumed that the zone is empty.
 * For example, if arch_max_dma_pfn == arch_max_dma32_pfn, it is assumed
 * that arch_max_dma32_pfn has no pages. It is also assumed that a zone
 * starts where the previous one ended. For example, ZONE_DMA32 starts
 * at arch_max_dma_pfn.
 */
void __init free_area_init_nodes(unsigned long *max_zone_pfn)
{
	unsigned long start_pfn, end_pfn;
	int i, nid;

	/* Record where the zone boundaries are */
	memset(arch_zone_lowest_possible_pfn, 0,
				sizeof(arch_zone_lowest_possible_pfn));
	memset(arch_zone_highest_possible_pfn, 0,
				sizeof(arch_zone_highest_possible_pfn));
	arch_zone_lowest_possible_pfn[0] = find_min_pfn_with_active_regions();
	arch_zone_highest_possible_pfn[0] = max_zone_pfn[0];
	for (i = 1; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;
		arch_zone_lowest_possible_pfn[i] =
			arch_zone_highest_possible_pfn[i-1];
		arch_zone_highest_possible_pfn[i] =
			max(max_zone_pfn[i], arch_zone_lowest_possible_pfn[i]);
	}
	arch_zone_lowest_possible_pfn[ZONE_MOVABLE] = 0;
	arch_zone_highest_possible_pfn[ZONE_MOVABLE] = 0;

	/* Find the PFNs that ZONE_MOVABLE begins at in each node */
	memset(zone_movable_pfn, 0, sizeof(zone_movable_pfn));
	find_zone_movable_pfns_for_nodes();

	/* Print out the zone ranges */
	printk("Zone ranges:\n");
	for (i = 0; i < MAX_NR_ZONES; i++) {
		if (i == ZONE_MOVABLE)
			continue;
		printk(KERN_CONT "  %-8s ", zone_names[i]);
		if (arch_zone_lowest_possible_pfn[i] ==
				arch_zone_highest_possible_pfn[i])
			printk(KERN_CONT "empty\n");
		else
			printk(KERN_CONT "[mem %0#10lx-%0#10lx]\n",
				arch_zone_lowest_possible_pfn[i] << PAGE_SHIFT,
				(arch_zone_highest_possible_pfn[i]
					<< PAGE_SHIFT) - 1);
	}

	/* Print out the PFNs ZONE_MOVABLE begins at in each node */
	printk("Movable zone start for each node\n");
	for (i = 0; i < MAX_NUMNODES; i++) {
		if (zone_movable_pfn[i])
			printk("  Node %d: %#010lx\n", i,
			       zone_movable_pfn[i] << PAGE_SHIFT);
	}

	/* Print out the early node map */
	printk("Early memory node ranges\n");
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid)
		printk("  node %3d: [mem %#010lx-%#010lx]\n", nid,
		       start_pfn << PAGE_SHIFT, (end_pfn << PAGE_SHIFT) - 1);

	/* Initialise every node */
	mminit_verify_pageflags_layout();
	setup_nr_node_ids();
	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		free_area_init_node(nid, NULL,
				find_min_pfn_for_node(nid), NULL);

		/* Any memory on that node */
		if (pgdat->node_present_pages)
			node_set_state(nid, N_MEMORY);
		check_for_memory(pgdat, nid);
	}
}

static int __init cmdline_parse_core(char *p, unsigned long *core)
{
	unsigned long long coremem;
	if (!p)
		return -EINVAL;

	coremem = memparse(p, &p);
	*core = coremem >> PAGE_SHIFT;

	/* Paranoid check that UL is enough for the coremem value */
	WARN_ON((coremem >> PAGE_SHIFT) > ULONG_MAX);

	return 0;
}

/*
 * kernelcore=size sets the amount of memory for use for allocations that
 * cannot be reclaimed or migrated.
 */
static int __init cmdline_parse_kernelcore(char *p)
{
	return cmdline_parse_core(p, &required_kernelcore);
}

/*
 * movablecore=size sets the amount of memory for use for allocations that
 * can be reclaimed or migrated.
 */
static int __init cmdline_parse_movablecore(char *p)
{
	return cmdline_parse_core(p, &required_movablecore);
}

early_param("kernelcore", cmdline_parse_kernelcore);
early_param("movablecore", cmdline_parse_movablecore);

#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

void adjust_managed_page_count(struct page *page, long count)
{
	spin_lock(&managed_page_count_lock);
	page_zone(page)->managed_pages += count;
	totalram_pages += count;
#ifdef CONFIG_HIGHMEM
	if (PageHighMem(page))
		totalhigh_pages += count;
#endif
	spin_unlock(&managed_page_count_lock);
}
EXPORT_SYMBOL(adjust_managed_page_count);

unsigned long free_reserved_area(void *start, void *end, int poison, char *s)
{
	void *pos;
	unsigned long pages = 0;

	start = (void *)PAGE_ALIGN((unsigned long)start);
	end = (void *)((unsigned long)end & PAGE_MASK);
	for (pos = start; pos < end; pos += PAGE_SIZE, pages++) {
		if ((unsigned int)poison <= 0xFF)
			memset(pos, poison, PAGE_SIZE);
		free_reserved_page(virt_to_page(pos));
	}

	if (pages && s)
		pr_info("Freeing %s memory: %ldK (%p - %p)\n",
			s, pages << (PAGE_SHIFT - 10), start, end);

	return pages;
}
EXPORT_SYMBOL(free_reserved_area);

#ifdef	CONFIG_HIGHMEM // CONFIG_HIGHMEM=y
// ARM10C 20140419
// pfn_to_page(0x4F800): 0x4F800 (pfn)
void free_highmem_page(struct page *page)
{
	// page: 0x4F800 (pfn)
	__free_reserved_page(page);
	// page를 order 0 으로 buddy에 추가.

	// totalram_pages: 총 free된 page 수 + 0x6
	totalram_pages++;
	// totalram_pages: 총 free된 page 수 + 0x6 + 1 (결국 free된 page 만큼 증가)

	// page_zone(page)->managed_pages: (&(&contig_page_data)->node_zones[1])->managed_pages
	page_zone(page)->managed_pages++;
	// higemem 영역, 결국 free된 page 만큼 managed_pages 증가

	// totalhigh_pages: 0
	totalhigh_pages++;
	// free된 page 만큼 totalhigh_pages 증가
}
#endif


// ARM10C 20140419
// str: NULL
void __init mem_init_print_info(const char *str)
{
	unsigned long physpages, codesize, datasize, rosize, bss_size;
	unsigned long init_code_size, init_data_size;

	// get_num_physpages(): 0x80000
	physpages = get_num_physpages();
        // physpages: 0x80000

	// System.map 에서 영역 계산 가능 (이하 적용)
	codesize = _etext - _stext;
	datasize = _edata - _sdata;
	rosize = __end_rodata - __start_rodata;
	bss_size = __bss_stop - __bss_start;
	init_data_size = __init_end - __init_begin;
	init_code_size = _einittext - _sinittext;

	/*
	 * Detect special cases and adjust section sizes accordingly:
	 * 1) .init.* may be embedded into .data sections
	 * 2) .init.text.* may be out of [__init_begin, __init_end],
	 *    please refer to arch/tile/kernel/vmlinux.lds.S.
	 * 3) .rodata.* may be embedded into .text or .data sections.
	 */

	// .init.* , .init.text.* , .rodata.* 섹션은 이후 다른 섹션으로 변경되기
	// 때문에 현재 section의 size를 미리 계산하여 로그용으로 사용.

#define adj_init_size(start, end, size, pos, adj) \
	do { \
		if (start <= pos && pos < end && size > adj) \
			size -= adj; \
	} while (0)

	adj_init_size(__init_begin, __init_end, init_data_size,
		     _sinittext, init_code_size);
	adj_init_size(_stext, _etext, codesize, _sinittext, init_code_size);
	adj_init_size(_sdata, _edata, datasize, __init_begin, init_data_size);
	adj_init_size(_stext, _etext, codesize, __start_rodata, rosize);
	adj_init_size(_sdata, _edata, datasize, __start_rodata, rosize);

#undef	adj_init_size

	printk("Memory: %luK/%luK available "
	       "(%luK kernel code, %luK rwdata, %luK rodata, "
	       "%luK init, %luK bss, %luK reserved"
#ifdef	CONFIG_HIGHMEM // CONFIG_HIGHMEM=y
	       ", %luK highmem"
#endif
	       "%s%s)\n",
	       // PAGE_SHIFT: 12
	       nr_free_pages() << (PAGE_SHIFT-10), physpages << (PAGE_SHIFT-10),
	       codesize >> 10, datasize >> 10, rosize >> 10,
	       (init_data_size + init_code_size) >> 10, bss_size >> 10,
	       (physpages - totalram_pages) << (PAGE_SHIFT-10),
#ifdef	CONFIG_HIGHMEM // CONFIG_HIGHMEM=y
	       totalhigh_pages << (PAGE_SHIFT-10),
#endif
	       str ? ", " : "", str ? str : "");
}

/**
 * set_dma_reserve - set the specified number of pages reserved in the first zone
 * @new_dma_reserve: The number of pages to mark reserved
 *
 * The per-cpu batchsize and zone watermarks are determined by present_pages.
 * In the DMA zone, a significant percentage may be consumed by kernel image
 * and other unfreeable allocations which can skew the watermarks badly. This
 * function may optionally be used to account for unfreeable pages in the
 * first zone (e.g., ZONE_DMA). The effect will be lower watermarks and
 * smaller per-cpu batchsize.
 */
void __init set_dma_reserve(unsigned long new_dma_reserve)
{
	dma_reserve = new_dma_reserve;
}

void __init free_area_init(unsigned long *zones_size)
{
	free_area_init_node(0, zones_size,
			__pa(PAGE_OFFSET) >> PAGE_SHIFT, NULL);
}

// ARM10C 20140315
static int page_alloc_cpu_notify(struct notifier_block *self,
				 unsigned long action, void *hcpu)
{
	int cpu = (unsigned long)hcpu;

	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		lru_add_drain_cpu(cpu);
		drain_pages(cpu);

		/*
		 * Spill the event counters of the dead processor
		 * into the current processors event counters.
		 * This artificially elevates the count of the current
		 * processor.
		 */
		vm_events_fold_cpu(cpu);

		/*
		 * Zero the differential counters of the dead processor
		 * so that the vm statistics are consistent.
		 *
		 * This is only okay since the processor is dead and cannot
		 * race with what we are doing.
		 */
		cpu_vm_stats_fold(cpu);
	}
	return NOTIFY_OK;
}

// ARM10C 20140315
void __init page_alloc_init(void)
{
	hotcpu_notifier(page_alloc_cpu_notify, 0);
}

/*
 * calculate_totalreserve_pages - called when sysctl_lower_zone_reserve_ratio
 *	or min_free_kbytes changes.
 */
static void calculate_totalreserve_pages(void)
{
	struct pglist_data *pgdat;
	unsigned long reserve_pages = 0;
	enum zone_type i, j;

	for_each_online_pgdat(pgdat) {
		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *zone = pgdat->node_zones + i;
			unsigned long max = 0;

			/* Find valid and maximum lowmem_reserve in the zone */
			for (j = i; j < MAX_NR_ZONES; j++) {
				if (zone->lowmem_reserve[j] > max)
					max = zone->lowmem_reserve[j];
			}

			/* we treat the high watermark as reserved pages. */
			max += high_wmark_pages(zone);

			if (max > zone->managed_pages)
				max = zone->managed_pages;
			reserve_pages += max;
			/*
			 * Lowmem reserves are not available to
			 * GFP_HIGHUSER page cache allocations and
			 * kswapd tries to balance zones to their high
			 * watermark.  As a result, neither should be
			 * regarded as dirtyable memory, to prevent a
			 * situation where reclaim has to clean pages
			 * in order to balance the zones.
			 */
			zone->dirty_balance_reserve = max;
		}
	}
	dirty_balance_reserve = reserve_pages;
	totalreserve_pages = reserve_pages;
}

/*
 * setup_per_zone_lowmem_reserve - called whenever
 *	sysctl_lower_zone_reserve_ratio changes.  Ensures that each zone
 *	has a correct pages reserved value, so an adequate number of
 *	pages are left in the zone after a successful __alloc_pages().
 */
static void setup_per_zone_lowmem_reserve(void)
{
	struct pglist_data *pgdat;
	enum zone_type j, idx;

	for_each_online_pgdat(pgdat) {
		for (j = 0; j < MAX_NR_ZONES; j++) {
			struct zone *zone = pgdat->node_zones + j;
			unsigned long managed_pages = zone->managed_pages;

			zone->lowmem_reserve[j] = 0;

			idx = j;
			while (idx) {
				struct zone *lower_zone;

				idx--;

				if (sysctl_lowmem_reserve_ratio[idx] < 1)
					sysctl_lowmem_reserve_ratio[idx] = 1;

				lower_zone = pgdat->node_zones + idx;
				lower_zone->lowmem_reserve[j] = managed_pages /
					sysctl_lowmem_reserve_ratio[idx];
				managed_pages += lower_zone->managed_pages;
			}
		}
	}

	/* update totalreserve_pages */
	calculate_totalreserve_pages();
}

static void __setup_per_zone_wmarks(void)
{
	unsigned long pages_min = min_free_kbytes >> (PAGE_SHIFT - 10);
	unsigned long lowmem_pages = 0;
	struct zone *zone;
	unsigned long flags;

	/* Calculate total number of !ZONE_HIGHMEM pages */
	for_each_zone(zone) {
		if (!is_highmem(zone))
			lowmem_pages += zone->managed_pages;
	}

	for_each_zone(zone) {
		u64 tmp;

		spin_lock_irqsave(&zone->lock, flags);
		tmp = (u64)pages_min * zone->managed_pages;
		do_div(tmp, lowmem_pages);
		if (is_highmem(zone)) {
			/*
			 * __GFP_HIGH and PF_MEMALLOC allocations usually don't
			 * need highmem pages, so cap pages_min to a small
			 * value here.
			 *
			 * The WMARK_HIGH-WMARK_LOW and (WMARK_LOW-WMARK_MIN)
			 * deltas controls asynch page reclaim, and so should
			 * not be capped for highmem.
			 */
			unsigned long min_pages;

			min_pages = zone->managed_pages / 1024;
			min_pages = clamp(min_pages, SWAP_CLUSTER_MAX, 128UL);
			zone->watermark[WMARK_MIN] = min_pages;
		} else {
			/*
			 * If it's a lowmem zone, reserve a number of pages
			 * proportionate to the zone's size.
			 */
			zone->watermark[WMARK_MIN] = tmp;
		}

		zone->watermark[WMARK_LOW]  = min_wmark_pages(zone) + (tmp >> 2);
		zone->watermark[WMARK_HIGH] = min_wmark_pages(zone) + (tmp >> 1);

		__mod_zone_page_state(zone, NR_ALLOC_BATCH,
				      high_wmark_pages(zone) -
				      low_wmark_pages(zone) -
				      zone_page_state(zone, NR_ALLOC_BATCH));

		setup_zone_migrate_reserve(zone);
		spin_unlock_irqrestore(&zone->lock, flags);
	}

	/* update totalreserve_pages */
	calculate_totalreserve_pages();
}

/**
 * setup_per_zone_wmarks - called when min_free_kbytes changes
 * or when memory is hot-{added|removed}
 *
 * Ensures that the watermark[min,low,high] values for each zone are set
 * correctly with respect to min_free_kbytes.
 */
void setup_per_zone_wmarks(void)
{
	mutex_lock(&zonelists_mutex);
	__setup_per_zone_wmarks();
	mutex_unlock(&zonelists_mutex);
}

/*
 * The inactive anon list should be small enough that the VM never has to
 * do too much work, but large enough that each inactive page has a chance
 * to be referenced again before it is swapped out.
 *
 * The inactive_anon ratio is the target ratio of ACTIVE_ANON to
 * INACTIVE_ANON pages on this zone's LRU, maintained by the
 * pageout code. A zone->inactive_ratio of 3 means 3:1 or 25% of
 * the anonymous pages are kept on the inactive list.
 *
 * total     target    max
 * memory    ratio     inactive anon
 * -------------------------------------
 *   10MB       1         5MB
 *  100MB       1        50MB
 *    1GB       3       250MB
 *   10GB      10       0.9GB
 *  100GB      31         3GB
 *    1TB     101        10GB
 *   10TB     320        32GB
 */
static void __meminit calculate_zone_inactive_ratio(struct zone *zone)
{
	unsigned int gb, ratio;

	/* Zone size in gigabytes */
	gb = zone->managed_pages >> (30 - PAGE_SHIFT);
	if (gb)
		ratio = int_sqrt(10 * gb);
	else
		ratio = 1;

	zone->inactive_ratio = ratio;
}

static void __meminit setup_per_zone_inactive_ratio(void)
{
	struct zone *zone;

	for_each_zone(zone)
		calculate_zone_inactive_ratio(zone);
}

/*
 * Initialise min_free_kbytes.
 *
 * For small machines we want it small (128k min).  For large machines
 * we want it large (64MB max).  But it is not linear, because network
 * bandwidth does not increase linearly with machine size.  We use
 *
 *	min_free_kbytes = 4 * sqrt(lowmem_kbytes), for better accuracy:
 *	min_free_kbytes = sqrt(lowmem_kbytes * 16)
 *
 * which yields
 *
 * 16MB:	512k
 * 32MB:	724k
 * 64MB:	1024k
 * 128MB:	1448k
 * 256MB:	2048k
 * 512MB:	2896k
 * 1024MB:	4096k
 * 2048MB:	5792k
 * 4096MB:	8192k
 * 8192MB:	11584k
 * 16384MB:	16384k
 */
int __meminit init_per_zone_wmark_min(void)
{
	unsigned long lowmem_kbytes;
	int new_min_free_kbytes;

	lowmem_kbytes = nr_free_buffer_pages() * (PAGE_SIZE >> 10);
	new_min_free_kbytes = int_sqrt(lowmem_kbytes * 16);

	if (new_min_free_kbytes > user_min_free_kbytes) {
		min_free_kbytes = new_min_free_kbytes;
		if (min_free_kbytes < 128)
			min_free_kbytes = 128;
		if (min_free_kbytes > 65536)
			min_free_kbytes = 65536;
	} else {
		pr_warn("min_free_kbytes is not updated to %d because user defined value %d is preferred\n",
				new_min_free_kbytes, user_min_free_kbytes);
	}
	setup_per_zone_wmarks();
	refresh_zone_stat_thresholds();
	setup_per_zone_lowmem_reserve();
	setup_per_zone_inactive_ratio();
	return 0;
}
module_init(init_per_zone_wmark_min)

/*
 * min_free_kbytes_sysctl_handler - just a wrapper around proc_dointvec() so
 *	that we can call two helper functions whenever min_free_kbytes
 *	changes.
 */
int min_free_kbytes_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec(table, write, buffer, length, ppos);
	if (write) {
		user_min_free_kbytes = min_free_kbytes;
		setup_per_zone_wmarks();
	}
	return 0;
}

#ifdef CONFIG_NUMA
int sysctl_min_unmapped_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	for_each_zone(zone)
		zone->min_unmapped_pages = (zone->managed_pages *
				sysctl_min_unmapped_ratio) / 100;
	return 0;
}

int sysctl_min_slab_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	int rc;

	rc = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (rc)
		return rc;

	for_each_zone(zone)
		zone->min_slab_pages = (zone->managed_pages *
				sysctl_min_slab_ratio) / 100;
	return 0;
}
#endif

/*
 * lowmem_reserve_ratio_sysctl_handler - just a wrapper around
 *	proc_dointvec() so that we can call setup_per_zone_lowmem_reserve()
 *	whenever sysctl_lowmem_reserve_ratio changes.
 *
 * The reserve ratio obviously has absolutely no relation with the
 * minimum watermarks. The lowmem reserve ratio can only make sense
 * if in function of the boot time zone sizes.
 */
int lowmem_reserve_ratio_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec_minmax(table, write, buffer, length, ppos);
	setup_per_zone_lowmem_reserve();
	return 0;
}

/*
 * percpu_pagelist_fraction - changes the pcp->high for each zone on each
 * cpu.  It is the fraction of total pages in each zone that a hot per cpu
 * pagelist can have before it gets flushed back to buddy allocator.
 */
int percpu_pagelist_fraction_sysctl_handler(ctl_table *table, int write,
	void __user *buffer, size_t *length, loff_t *ppos)
{
	struct zone *zone;
	unsigned int cpu;
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (!write || (ret < 0))
		return ret;

	mutex_lock(&pcp_batch_high_lock);
	for_each_populated_zone(zone) {
		unsigned long  high;
		high = zone->managed_pages / percpu_pagelist_fraction;
		for_each_possible_cpu(cpu)
			pageset_set_high(per_cpu_ptr(zone->pageset, cpu),
					 high);
	}
	mutex_unlock(&pcp_batch_high_lock);
	return 0;
}

// ARM10C 20140322
// ARM10C 20151003
// HASHDIST_DEFAULT: 0
// hashdist: 0
int hashdist = HASHDIST_DEFAULT;

#ifdef CONFIG_NUMA
static int __init set_hashdist(char *str)
{
	if (!str)
		return 0;
	hashdist = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("hashdist=", set_hashdist);
#endif

/*
 * allocate a large system hash table from bootmem
 * - it is assumed that the hash table must contain an exact power-of-2
 *   quantity of entries
 * - limit is the number of hash buckets, not the total allocation size
 */

// ARM10C 20140322
// [PID] tablename : "PID", bucketsize : sizeof(*pid_hash) : 4,
// numentries :  0, scale : 18,
// flags : 0x00000003 : HASH_EARLY : 0x00000001 | HASH_SMALL : 0x00000002,
// *_hash_shift : &pidhash_shift : 4, *_hash_mask : NULL,
// low_limit : 0, high_limit: 4096

// ARM10C 20140322
// [dCA] tablename : "Dentry cache", bucketsize : 4 : sizeof(struct hlist_bl_head),
// numentries : 0 : dhash_entries, scale : 13
// flags : HASH_EARLY : 0x00000001
// *_hash_shift : &d_hash_shift, *_hash_mask : &d_hash_mask,
// low_limit : 	0, high_limit : 0

// ARM10C 20140322
// [iCA] tablename : "Inode-cache", bucketsize : 4 : sizeof(struct hlist_bl_head),
// numentries : 0 : ihash_entries, scale : 14
// flags : HASH_EARLY : 0x00000001
// *_hash_shift : &i_hash_shift, *_hash_mask : &i_hash_mask,
// low_limit : 	0, high_limit : 0

void *__init alloc_large_system_hash(const char *tablename,
				     unsigned long bucketsize,
				     unsigned long numentries,
				     int scale,
				     int flags,
				     unsigned int *_hash_shift,
				     unsigned int *_hash_mask,
				     unsigned long low_limit,
				     unsigned long high_limit)
{
	unsigned long long max = high_limit;
	// [PID] max : 4096
	// [dCA] max : 0
	// [iCA] max : 0
	unsigned long log2qty, size;
	void *table = NULL;

	/* allow the kernel cmdline to have a say */
	if (!numentries) {
		// [PID] numentries : 0 이므로 if문 실행
		// [dCA] numentries : 0 이므로 if문 실행
		// [iCA] numentries : 0 이므로 if문 실행

		/* round applicable memory size up to nearest megabyte */
		// [PID] static unsigned long __meminitdata nr_kernel_pages
		// [PID] nr_kernel_pages : 0x2EFD6
		// [dCA] static unsigned long __meminitdata nr_kernel_pages
		// [dCA] nr_kernel_pages : 0x2EFD6
		// [iCA] static unsigned long __meminitdata nr_kernel_pages
		// [iCA] nr_kernel_pages : 0x2EFD6
		numentries = nr_kernel_pages;
		// [PID] numentries : 0x2EFD6
		// [dCA] numentries : 0x2EFD6
		// [iCA] numentries : 0x2EFD6

		/* It isn't necessary when PAGE_SIZE >= 1MB */
		if (PAGE_SHIFT < 20)
		// [PID] numentries : 0x2EFD6
		// [dCA] numentries : 0x2EFD6
		// [iCA] numentries : 0x2EFD6
			// PAGE_SIZE: 0x1000, (1UL << 20) / 0x1000): 0xFF
			numentries = round_up(numentries, (1<<20)/PAGE_SIZE);
		// [PID] numentries : 0x2F000
		// [dCA] numentries : 0x2F000
		// [iCA] numentries : 0x2F000

		/* limit to 1 bucket per 2^scale bytes of low memory */
		// [PID] scale : (18 > 12)
		// [dCA] scale : (13 > 12)
		// [iCA] scale : (14 > 12)
		if (scale > PAGE_SHIFT)
			numentries >>= (scale - PAGE_SHIFT);
			// [PID] numentries >>= (scale : 18 - PAGE_SHIFT: 12)
			// [PID] numentries : 0xBC0 : 0x2F000 >> 6:
			// [dCA] numentries >>= (scale : 13 - PAGE_SHIFT: 12)
			// [dCA] numentries : 0x17800 : 0x2F000 >> 1:
			// [iCA] numentries >>= (scale : 14 - PAGE_SHIFT: 12)
			// [iCA] numentries : 0xBC00 : 0x2F000 >> 2:
		else
			numentries <<= (PAGE_SHIFT - scale);

		/* Make sure we've got at least a 0-order allocation.. */
		// [PID] unlikely (0x00000002) : flags : 0x00000003 & HASH_SMALL 0x00000002
		// [dCA] unlikely (0x00000000) : flags : 0x00000001 & HASH_SMALL 0x00000002
		// [iCA] unlikely (0x00000000) : flags : 0x00000001 & HASH_SMALL 0x00000002
		if (unlikely(flags & HASH_SMALL)) {
			/* Makes no sense without HASH_EARLY */
		        // [PID] flags : 0x00000003 & HASH_EARLY : 0x00000001
			WARN_ON(!(flags & HASH_EARLY));

			// [PID] numentries : 0xBC : 0xBC0 >> 4, *_hash_shift: 4
			if (!(numentries >> *_hash_shift)) {
				numentries = 1UL << *_hash_shift;
				BUG_ON(!numentries);
			}
		} else if (unlikely((numentries * bucketsize) < PAGE_SIZE))
		// [dCA] unlikely(0x5E000 : (0x17800 * 4) < 0x1000)
		// [iCA] unlikely(0x2F000 : (0xBC00 * 4) < 0x1000)
			numentries = PAGE_SIZE / bucketsize;
	}

	// [PID] numentries : 0xBC0 : 3008  
	// [dCA] numentries : 0x17800 : 96256
	// [iCA] numentries : 0xBC00 : 48128	
	numentries = roundup_pow_of_two(numentries);
	// [PID] numentries : 4096
	// [dCA] numentries : 131072
	// [iCA] numentries : 65536
	
	/* limit allocation size to 1/16 total memory by default */
	// [PID] max : 4096
	// [dCA] max : 0
	// [iCA] max : 0
	if (max == 0) {
		// nr_all_pages: 0x7EA00, PAGE_SHIFT: 12
		max = ((unsigned long long)nr_all_pages << PAGE_SHIFT) >> 4;
		// max = (nr_all_pages : 0x7EA0000 : 0x7EA00000 >> 4 : 0x7EA00  << 12) >> 4

		// max : 0x7EA0000, bucketsize : 4
		do_div(max, bucketsize);
		// max : 0x1FA8000
	}

	// [PID] max : 4096 : min(4096, 0x80000000ULL)
	// [dCA] max : 0x1FA8000 : min(0x1FA8000, 0x80000000ULL)
	// [iCA] max : 0x1FA8000 : min(0x1FA8000, 0x80000000ULL)
	max = min(max, 0x80000000ULL);
	// [PID] max : 4096
	// [dCA] max : 0x1FA8000
	// [iCA] max : 0x1FA8000

	// [PID] numentries :   4096 < 0
	// [dCA] numentries : 131072 < 0
	// [iCA] numentries :  65536 < 0
	if (numentries < low_limit)
		numentries = low_limit;

 	// [PID] numentries :   4096 >    4096
	// [dCA] numentries : 131072 > 33193984 : 0x1FA8000
	// [iCA] numentries :  65536 > 33193984 : 0x1FA8000
	if (numentries > max)
		numentries = max;

	// [PID] numentries :   4096
	// [dCA] numentries : 131072
	// [iCA] numentries :  65536
	log2qty = ilog2(numentries);
	// [PID] log2qty : 12 : ilog2(4096)
	// [dCA] log2qty : 17 : ilog2(131072)
	// [iCA] log2qty : 16 : ilog2(65536)
	
	do {
		// bucketsize: 4
		size = bucketsize << log2qty;
		// [PID] size = 0x4000 (16kB) : 4 << 12
		// [dCA] size = 0x80000 (512kB) : 4 << 17
		// [iCA] size = 0x40000 (256kB) : 4 << 16

		// [PID] 0x00000001 : flags : 0x00000003 & HASH_EARLY : 0x00000001
		// [dCA] 0x00000001 : flags : 0x00000001 & HASH_EARLY : 0x00000001
		// [iCA] 0x00000001 : flags : 0x00000001 & HASH_EARLY : 0x00000001
		if (flags & HASH_EARLY)
			// [PID] size : 0x4000 (16kB)
			// [dCA] size : 0x80000 (512kB)
			// [iCA] size : 0x40000 (256kB)
			table = alloc_bootmem_nopanic(size);
			// [PID] table : 16kB만큼 할당받은 메모리 주소
			// [dCA] table : 512kB만큼 할당받은 메모리 주소		
			// [iCA] table : 256kB만큼 할당받은 메모리 주소		
		else if (hashdist)
			table = __vmalloc(size, GFP_ATOMIC, PAGE_KERNEL);
		else {
			/*
			 * If bucketsize is not a power-of-two, we may free
			 * some pages at the end of hash table which
			 * alloc_pages_exact() automatically does
			 */
			if (get_order(size) < MAX_ORDER) {
				table = alloc_pages_exact(size, GFP_ATOMIC);
				kmemleak_alloc(table, size, 1, GFP_ATOMIC);
			}
		}
	} while (!table && size > PAGE_SIZE && --log2qty);
    
	// [PID] table : 16kB만큼 할당받은 메모리 주소
	// [dCA] table : 512kB만큼 할당받은 메모리 주소		
	// [iCA] table : 256kB만큼 할당받은 메모리 주소		
	if (!table)
		panic("Failed to allocate %s hash table\n", tablename);

	printk(KERN_INFO "%s hash table entries: %ld (order: %d, %lu bytes)\n",
	       tablename,
	       (1UL << log2qty),
	       ilog2(size) - PAGE_SHIFT,
	       size);
	// [PID] PID hash table entries: 4096 (order: 2, 4096 bytes)
	// [dCA] Dentry cache hash table entries: 131072 (order: 7, 524288 bytes)
	// [iCA] Inode-cache hash table entries: 65536 (order: 6, 262144 bytes)	

	// [PID] _hash_shift : NULL아닌 주소
	// [dCA] _hash_shift : NULL아닌 주소
	// [iCA] _hash_shift : NULL아닌 주소
	if (_hash_shift)
		// [PID] log2qty : 12
		// [dCA] log2qty : 17
		// [iCA] log2qty : 16
		*_hash_shift = log2qty;
		// [PID] *_hash_shift : 12
		// [dCA] *_hash_shift : 17
		// [iCA] *_hash_shift : 16

	// [PID] _hash_mask : NULL 이므로 if문은 패스
	if (_hash_mask)
		// [dCA] log2qty : 17
		// [iCA] log2qty : 16
		*_hash_mask = (1 << log2qty) - 1;
		// [dCA] *_hash_mask : (131071) 0x1FFFF : (1 << 17 ) - 1
		// [iCA] *_hash_mask : (65535) 0xFFFF : (1 << 16 ) - 1

	return table;
}

/* Return a pointer to the bitmap storing bits affecting a block of pages */
// ARM10C 20140118
// zone : &contig_page_data.node_zones[0], pfn : 0x20000
static inline unsigned long *get_pageblock_bitmap(struct zone *zone,
							unsigned long pfn)
{
#ifdef CONFIG_SPARSEMEM
	// &mem_section[0][2]->pageblock_flags
	return __pfn_to_section(pfn)->pageblock_flags;
	// 4KB 할당받아뒀던 주소 값을 리턴 : sparse_early_usemaps_alloc_node()
#else
	return zone->pageblock_flags;
#endif /* CONFIG_SPARSEMEM */
}

// ARM10C 20140118
// zone : &contig_page_data.node_zones[0], pfn : 0x20000
// zone : &contig_page_data.node_zones[1], pfn : 0x4F800
static inline int pfn_to_bitidx(struct zone *zone, unsigned long pfn)
{
#ifdef CONFIG_SPARSEMEM // CONFIG_SPARSEMEM=y
	// PAGES_PER_SECTION : 0x10000
	// pfn : 0 (section의 몇 번째 프레임)
	// pfn : 0xF800
	pfn &= (PAGES_PER_SECTION-1);
	// pageblock_order: 10, NR_PAGEBLOCK_BITS : 4
	// bit : page 1024개당 4 비트씩 사용
	return (pfn >> pageblock_order) * NR_PAGEBLOCK_BITS;
	// return 0x0
	// return 0xC8
#else
	pfn = pfn - round_down(zone->zone_start_pfn, pageblock_nr_pages);
	return (pfn >> pageblock_order) * NR_PAGEBLOCK_BITS;
#endif /* CONFIG_SPARSEMEM */
}

/**
 * get_pageblock_flags_group - Return the requested group of flags for the pageblock_nr_pages block of pages
 * @page: The page within the block of interest
 * @start_bitidx: The first bit of interest to retrieve
 * @end_bitidx: The last bit of interest
 * returns pageblock_bits flags
 */
// ARM10C 20140405
// page: 0x20000 (pfn), PB_migrate: 0, PB_migrate_end: 2
unsigned long get_pageblock_flags_group(struct page *page,
					int start_bitidx, int end_bitidx)
{
	struct zone *zone;
	unsigned long *bitmap;
	unsigned long pfn, bitidx;
	unsigned long flags = 0;
	unsigned long value = 1;

	// page: 0x20000 (pfn)
	zone = page_zone(page);
	// zone: (&contig_page_data)->node_zones[0]

	// page: 0x20000 (pfn)
	pfn = page_to_pfn(page);
	// pfn: 0x20000

	// zone: (&contig_page_data)->node_zones[0]
	bitmap = get_pageblock_bitmap(zone, pfn);
	// bitmap: &mem_section[0][2]->pageblock_flags
	// pageblock bitmap의 주소

	// zone: (&contig_page_data)->node_zones[0], pfn: 0x20000
	bitidx = pfn_to_bitidx(zone, pfn);
	// bitidx: 0

	// start_bitidx: 0, end_bitidx: 2, value: 1, bitidx: 0
	// memmap_init_zone 에서 pageblock_flags의 MIGRATE_MOVABLE(2)번 비트를 1로 설정
	// 1024번째 page마다 수행됨
	for (; start_bitidx <= end_bitidx; start_bitidx++, value <<= 1)
		if (test_bit(bitidx + start_bitidx, bitmap))
			flags |= value;
	// flags: 0x2

	return flags;
}

/**
 * set_pageblock_flags_group - Set the requested group of flags for a pageblock_nr_pages block of pages
 * @page: The page within the block of interest
 * @start_bitidx: The first bit of interest
 * @end_bitidx: The last bit of interest
 * @flags: The flags to set
 */
// ARM10C 20140118
// migratetype : 2, PB_migrate : 0, PB_migrate_end : 2
// ARM10C 20140517
// migratetype : 0, PB_migrate : 0, PB_migrate_end : 2
void set_pageblock_flags_group(struct page *page, unsigned long flags,
					int start_bitidx, int end_bitidx)
{
	struct zone *zone;
	unsigned long *bitmap;
	unsigned long pfn, bitidx;
	unsigned long value = 1;

	// page : ??, flags : 2, start_bitidx : 0, end_bitidx : 2
	// zone : &contig_page_data.node_zones[0]
	// zone : &contig_page_data.node_zones[1]
	zone = page_zone(page);
	// pfn offset이 계산됨
	// pfn : 0x20000
	// pfn : 0x4F800
	pfn = page_to_pfn(page);
	// zone : &contig_page_data.node_zones[0], pfn : 0x20000
	// bitmap : &mem_section[0][2]->pageblock_flags
	// zone : &contig_page_data.node_zones[1], pfn : 0x4F800
	// bitmap : &mem_section[0][4]->pageblock_flags
	bitmap = get_pageblock_bitmap(zone, pfn);
	// zone : &contig_page_data.node_zones[0], pfn : 0x20000
	// bitidx : 0
	// zone : &contig_page_data.node_zones[1], pfn : 0x4F800
	// bitidx : 0xC8
	bitidx = pfn_to_bitidx(zone, pfn);
	VM_BUG_ON(!zone_spans_pfn(zone, pfn));
	
	// start_bitidx : 0, end_bitidx : 2
	for (; start_bitidx <= end_bitidx; start_bitidx++, value <<= 1)
		// flags : 2, value : 1
		// flags : 2, value : 2
		if (flags & value)
			// bitidx + start_bitidx : 1, bitmap : &mem_section[0][2]->pageblock_flags
			// bitidx + start_bitidx : 1, bitmap : &mem_section[0][4]->pageblock_flags
			__set_bit(bitidx + start_bitidx, bitmap);
		else
			// bitidx + start_bitidx : 0, bitmap : &mem_section[0][2]->pageblock_flags
			// bitidx + start_bitidx : 0, bitmap : &mem_section[0][4]->pageblock_flags
			__clear_bit(bitidx + start_bitidx, bitmap);
}

/*
 * This function checks whether pageblock includes unmovable pages or not.
 * If @count is not zero, it is okay to include less @count unmovable pages
 *
 * PageLRU check without isolation or lru_lock could race so that
 * MIGRATE_MOVABLE block might include unmovable pages. It means you can't
 * expect this function should be exact.
 */
bool has_unmovable_pages(struct zone *zone, struct page *page, int count,
			 bool skip_hwpoisoned_pages)
{
	unsigned long pfn, iter, found;
	int mt;

	/*
	 * For avoiding noise data, lru_add_drain_all() should be called
	 * If ZONE_MOVABLE, the zone never contains unmovable pages
	 */
	if (zone_idx(zone) == ZONE_MOVABLE)
		return false;
	mt = get_pageblock_migratetype(page);
	if (mt == MIGRATE_MOVABLE || is_migrate_cma(mt))
		return false;

	pfn = page_to_pfn(page);
	for (found = 0, iter = 0; iter < pageblock_nr_pages; iter++) {
		unsigned long check = pfn + iter;

		if (!pfn_valid_within(check))
			continue;

		page = pfn_to_page(check);

		/*
		 * Hugepages are not in LRU lists, but they're movable.
		 * We need not scan over tail pages bacause we don't
		 * handle each tail page individually in migration.
		 */
		if (PageHuge(page)) {
			iter = round_up(iter + 1, 1<<compound_order(page)) - 1;
			continue;
		}

		/*
		 * We can't use page_count without pin a page
		 * because another CPU can free compound page.
		 * This check already skips compound tails of THP
		 * because their page->_count is zero at all time.
		 */
		if (!atomic_read(&page->_count)) {
			if (PageBuddy(page))
				iter += (1 << page_order(page)) - 1;
			continue;
		}

		/*
		 * The HWPoisoned page may be not in buddy system, and
		 * page_count() is not 0.
		 */
		if (skip_hwpoisoned_pages && PageHWPoison(page))
			continue;

		if (!PageLRU(page))
			found++;
		/*
		 * If there are RECLAIMABLE pages, we need to check it.
		 * But now, memory offline itself doesn't call shrink_slab()
		 * and it still to be fixed.
		 */
		/*
		 * If the page is not RAM, page_count()should be 0.
		 * we don't need more check. This is an _used_ not-movable page.
		 *
		 * The problematic thing here is PG_reserved pages. PG_reserved
		 * is set to both of a memory hole page and a _used_ kernel
		 * page at boot.
		 */
		if (found > count)
			return true;
	}
	return false;
}

bool is_pageblock_removable_nolock(struct page *page)
{
	struct zone *zone;
	unsigned long pfn;

	/*
	 * We have to be careful here because we are iterating over memory
	 * sections which are not zone aware so we might end up outside of
	 * the zone but still within the section.
	 * We have to take care about the node as well. If the node is offline
	 * its NODE_DATA will be NULL - see page_zone.
	 */
	if (!node_online(page_to_nid(page)))
		return false;

	zone = page_zone(page);
	pfn = page_to_pfn(page);
	if (!zone_spans_pfn(zone, pfn))
		return false;

	return !has_unmovable_pages(zone, page, 0, true);
}

#ifdef CONFIG_CMA

static unsigned long pfn_max_align_down(unsigned long pfn)
{
	return pfn & ~(max_t(unsigned long, MAX_ORDER_NR_PAGES,
			     pageblock_nr_pages) - 1);
}

static unsigned long pfn_max_align_up(unsigned long pfn)
{
	return ALIGN(pfn, max_t(unsigned long, MAX_ORDER_NR_PAGES,
				pageblock_nr_pages));
}

/* [start, end) must belong to a single zone. */
static int __alloc_contig_migrate_range(struct compact_control *cc,
					unsigned long start, unsigned long end)
{
	/* This function is based on compact_zone() from compaction.c. */
	unsigned long nr_reclaimed;
	unsigned long pfn = start;
	unsigned int tries = 0;
	int ret = 0;

	migrate_prep();

	while (pfn < end || !list_empty(&cc->migratepages)) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		if (list_empty(&cc->migratepages)) {
			cc->nr_migratepages = 0;
			pfn = isolate_migratepages_range(cc->zone, cc,
							 pfn, end, true);
			if (!pfn) {
				ret = -EINTR;
				break;
			}
			tries = 0;
		} else if (++tries == 5) {
			ret = ret < 0 ? ret : -EBUSY;
			break;
		}

		nr_reclaimed = reclaim_clean_pages_from_list(cc->zone,
							&cc->migratepages);
		cc->nr_migratepages -= nr_reclaimed;

		ret = migrate_pages(&cc->migratepages, alloc_migrate_target,
				    0, MIGRATE_SYNC, MR_CMA);
	}
	if (ret < 0) {
		putback_movable_pages(&cc->migratepages);
		return ret;
	}
	return 0;
}

/**
 * alloc_contig_range() -- tries to allocate given range of pages
 * @start:	start PFN to allocate
 * @end:	one-past-the-last PFN to allocate
 * @migratetype:	migratetype of the underlaying pageblocks (either
 *			#MIGRATE_MOVABLE or #MIGRATE_CMA).  All pageblocks
 *			in range must have the same migratetype and it must
 *			be either of the two.
 *
 * The PFN range does not have to be pageblock or MAX_ORDER_NR_PAGES
 * aligned, however it's the caller's responsibility to guarantee that
 * we are the only thread that changes migrate type of pageblocks the
 * pages fall in.
 *
 * The PFN range must belong to a single zone.
 *
 * Returns zero on success or negative error code.  On success all
 * pages which PFN is in [start, end) are allocated for the caller and
 * need to be freed with free_contig_range().
 */
int alloc_contig_range(unsigned long start, unsigned long end,
		       unsigned migratetype)
{
	unsigned long outer_start, outer_end;
	int ret = 0, order;

	struct compact_control cc = {
		.nr_migratepages = 0,
		.order = -1,
		.zone = page_zone(pfn_to_page(start)),
		.sync = true,
		.ignore_skip_hint = true,
	};
	INIT_LIST_HEAD(&cc.migratepages);

	/*
	 * What we do here is we mark all pageblocks in range as
	 * MIGRATE_ISOLATE.  Because pageblock and max order pages may
	 * have different sizes, and due to the way page allocator
	 * work, we align the range to biggest of the two pages so
	 * that page allocator won't try to merge buddies from
	 * different pageblocks and change MIGRATE_ISOLATE to some
	 * other migration type.
	 *
	 * Once the pageblocks are marked as MIGRATE_ISOLATE, we
	 * migrate the pages from an unaligned range (ie. pages that
	 * we are interested in).  This will put all the pages in
	 * range back to page allocator as MIGRATE_ISOLATE.
	 *
	 * When this is done, we take the pages in range from page
	 * allocator removing them from the buddy system.  This way
	 * page allocator will never consider using them.
	 *
	 * This lets us mark the pageblocks back as
	 * MIGRATE_CMA/MIGRATE_MOVABLE so that free pages in the
	 * aligned range but not in the unaligned, original range are
	 * put back to page allocator so that buddy can use them.
	 */

	ret = start_isolate_page_range(pfn_max_align_down(start),
				       pfn_max_align_up(end), migratetype,
				       false);
	if (ret)
		return ret;

	ret = __alloc_contig_migrate_range(&cc, start, end);
	if (ret)
		goto done;

	/*
	 * Pages from [start, end) are within a MAX_ORDER_NR_PAGES
	 * aligned blocks that are marked as MIGRATE_ISOLATE.  What's
	 * more, all pages in [start, end) are free in page allocator.
	 * What we are going to do is to allocate all pages from
	 * [start, end) (that is remove them from page allocator).
	 *
	 * The only problem is that pages at the beginning and at the
	 * end of interesting range may be not aligned with pages that
	 * page allocator holds, ie. they can be part of higher order
	 * pages.  Because of this, we reserve the bigger range and
	 * once this is done free the pages we are not interested in.
	 *
	 * We don't have to hold zone->lock here because the pages are
	 * isolated thus they won't get removed from buddy.
	 */

	lru_add_drain_all();
	drain_all_pages();

	order = 0;
	outer_start = start;
	while (!PageBuddy(pfn_to_page(outer_start))) {
		if (++order >= MAX_ORDER) {
			ret = -EBUSY;
			goto done;
		}
		outer_start &= ~0UL << order;
	}

	/* Make sure the range is really isolated. */
	if (test_pages_isolated(outer_start, end, false)) {
		pr_warn("alloc_contig_range test_pages_isolated(%lx, %lx) failed\n",
		       outer_start, end);
		ret = -EBUSY;
		goto done;
	}


	/* Grab isolated pages from freelists. */
	outer_end = isolate_freepages_range(&cc, outer_start, end);
	if (!outer_end) {
		ret = -EBUSY;
		goto done;
	}

	/* Free head and tail (if any) */
	if (start != outer_start)
		free_contig_range(outer_start, start - outer_start);
	if (end != outer_end)
		free_contig_range(end, outer_end - end);

done:
	undo_isolate_page_range(pfn_max_align_down(start),
				pfn_max_align_up(end), migratetype);
	return ret;
}

void free_contig_range(unsigned long pfn, unsigned nr_pages)
{
	unsigned int count = 0;

	for (; nr_pages--; pfn++) {
		struct page *page = pfn_to_page(pfn);

		count += page_count(page) != 1;
		__free_page(page);
	}
	WARN(count != 0, "%d pages are still in use!\n", count);
}
#endif

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * The zone indicated has a new number of managed_pages; batch sizes and percpu
 * page high values need to be recalulated.
 */
void __meminit zone_pcp_update(struct zone *zone)
{
	unsigned cpu;
	mutex_lock(&pcp_batch_high_lock);
	for_each_possible_cpu(cpu)
		pageset_set_high_and_batch(zone,
				per_cpu_ptr(zone->pageset, cpu));
	mutex_unlock(&pcp_batch_high_lock);
}
#endif

void zone_pcp_reset(struct zone *zone)
{
	unsigned long flags;
	int cpu;
	struct per_cpu_pageset *pset;

	/* avoid races with drain_pages()  */
	local_irq_save(flags);
	if (zone->pageset != &boot_pageset) {
		for_each_online_cpu(cpu) {
			pset = per_cpu_ptr(zone->pageset, cpu);
			drain_zonestat(zone, pset);
		}
		free_percpu(zone->pageset);
		zone->pageset = &boot_pageset;
	}
	local_irq_restore(flags);
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * All pages in the range must be isolated before calling this.
 */
void
__offline_isolated_pages(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *page;
	struct zone *zone;
	int order, i;
	unsigned long pfn;
	unsigned long flags;
	/* find the first valid pfn */
	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		if (pfn_valid(pfn))
			break;
	if (pfn == end_pfn)
		return;
	zone = page_zone(pfn_to_page(pfn));
	spin_lock_irqsave(&zone->lock, flags);
	pfn = start_pfn;
	while (pfn < end_pfn) {
		if (!pfn_valid(pfn)) {
			pfn++;
			continue;
		}
		page = pfn_to_page(pfn);
		/*
		 * The HWPoisoned page may be not in buddy system, and
		 * page_count() is not 0.
		 */
		if (unlikely(!PageBuddy(page) && PageHWPoison(page))) {
			pfn++;
			SetPageReserved(page);
			continue;
		}

		BUG_ON(page_count(page));
		BUG_ON(!PageBuddy(page));
		order = page_order(page);
#ifdef CONFIG_DEBUG_VM
		printk(KERN_INFO "remove from free list %lx %d %lx\n",
		       pfn, 1 << order, end_pfn);
#endif
		list_del(&page->lru);
		rmv_page_order(page);
		zone->free_area[order].nr_free--;
		for (i = 0; i < (1 << order); i++)
			SetPageReserved((page+i));
		pfn += (1 << order);
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}
#endif

#ifdef CONFIG_MEMORY_FAILURE
bool is_free_buddy_page(struct page *page)
{
	struct zone *zone = page_zone(page);
	unsigned long pfn = page_to_pfn(page);
	unsigned long flags;
	int order;

	spin_lock_irqsave(&zone->lock, flags);
	for (order = 0; order < MAX_ORDER; order++) {
		struct page *page_head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(page_head) && page_order(page_head) >= order)
			break;
	}
	spin_unlock_irqrestore(&zone->lock, flags);

	return order < MAX_ORDER;
}
#endif

static const struct trace_print_flags pageflag_names[] = {
	{1UL << PG_locked,		"locked"	},
	{1UL << PG_error,		"error"		},
	{1UL << PG_referenced,		"referenced"	},
	{1UL << PG_uptodate,		"uptodate"	},
	{1UL << PG_dirty,		"dirty"		},
	{1UL << PG_lru,			"lru"		},
	{1UL << PG_active,		"active"	},
	{1UL << PG_slab,		"slab"		},
	{1UL << PG_owner_priv_1,	"owner_priv_1"	},
	{1UL << PG_arch_1,		"arch_1"	},
	{1UL << PG_reserved,		"reserved"	},
	{1UL << PG_private,		"private"	},
	{1UL << PG_private_2,		"private_2"	},
	{1UL << PG_writeback,		"writeback"	},
#ifdef CONFIG_PAGEFLAGS_EXTENDED
	{1UL << PG_head,		"head"		},
	{1UL << PG_tail,		"tail"		},
#else
	{1UL << PG_compound,		"compound"	},
#endif
	{1UL << PG_swapcache,		"swapcache"	},
	{1UL << PG_mappedtodisk,	"mappedtodisk"	},
	{1UL << PG_reclaim,		"reclaim"	},
	{1UL << PG_swapbacked,		"swapbacked"	},
	{1UL << PG_unevictable,		"unevictable"	},
#ifdef CONFIG_MMU
	{1UL << PG_mlocked,		"mlocked"	},
#endif
#ifdef CONFIG_ARCH_USES_PG_UNCACHED
	{1UL << PG_uncached,		"uncached"	},
#endif
#ifdef CONFIG_MEMORY_FAILURE
	{1UL << PG_hwpoison,		"hwpoison"	},
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	{1UL << PG_compound_lock,	"compound_lock"	},
#endif
};

static void dump_page_flags(unsigned long flags)
{
	const char *delim = "";
	unsigned long mask;
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(pageflag_names) != __NR_PAGEFLAGS);

	printk(KERN_ALERT "page flags: %#lx(", flags);

	/* remove zone id */
	flags &= (1UL << NR_PAGEFLAGS) - 1;

	for (i = 0; i < ARRAY_SIZE(pageflag_names) && flags; i++) {

		mask = pageflag_names[i].mask;
		if ((flags & mask) != mask)
			continue;

		flags &= ~mask;
		printk("%s%s", delim, pageflag_names[i].name);
		delim = "|";
	}

	/* check for left over flags */
	if (flags)
		printk("%s%#lx", delim, flags);

	printk(")\n");
}

void dump_page(struct page *page)
{
	printk(KERN_ALERT
	       "page:%p count:%d mapcount:%d mapping:%p index:%#lx\n",
		page, atomic_read(&page->_count), page_mapcount(page),
		page->mapping, page->index);
	dump_page_flags(page->flags);
	mem_cgroup_print_bad_page(page);
}
