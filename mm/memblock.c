/*
 * Procedures for maintaining information about logical memory blocks.
 *
 * Peter Bergner, IBM Corp.	June 2001.
 * Copyright (C) 2001 Peter Bergner.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/poison.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/memblock.h>

#include <asm-generic/sections.h>

// ARM10C 20131019
// KID 20140307
// ARM10C 20140419
// INIT_MEMBLOCK_REGIONS: 128
// memblock_memory_init_regions[0].base: 0x20000000
// memblock_memory_init_regions[0].size: 0x80000000
// memblock_memory_init_regions[1].base: 0x4f800000
// memblock_memory_init_regions[1].size: 0x50800000
static struct memblock_region memblock_memory_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;
// KID 20140311
// INIT_MEMBLOCK_REGIONS: 128
// memblock_reserved_init_regions[0].base: 0x20004000
// memblock_reserved_init_regions[0].size: 0x4000
// memblock_reserved_init_regions[1].base: 0x20008000
// memblock_reserved_init_regions[1].size: 0x0051ED20
static struct memblock_region memblock_reserved_init_regions[INIT_MEMBLOCK_REGIONS] __initdata_memblock;

// ARM10C 20131019
// ARM10C 20131026
// KID 20140307
// KID 20140311
// ARM10C 20140419
// memblock.current_limit: 0x4f800000
// memblock.memory.total_size: 0x80000000
// memblock.memory.cnt: 1
// memblock.memory.regions[0].base: 0x20000000
// memblock.memory.regions[0].size: 0x80000000
// memblock.memory.regions[1].base: 0x4f800000
// memblock.memory.regions[1].size: 0x50800000
// memblock.reserved.total_size: 0x00522D20
// memblock.reserved.cnt: 1
// memblock.reserved.regions[0].base: 0x20004000
// memblock.reserved.regions[0].size: 0x4000
// memblock.reserved.regions[1].base: 0x20008000
// memblock.reserved.regions[1].size: 0x0051ED20
struct memblock memblock __initdata_memblock = {
	.memory.regions		= memblock_memory_init_regions,
	.memory.cnt		= 1,	/* empty dummy entry */
	// INIT_MEMBLOCK_REGIONS: 128
	.memory.max		= INIT_MEMBLOCK_REGIONS,

	.reserved.regions	= memblock_reserved_init_regions,
	.reserved.cnt		= 1,	/* empty dummy entry */
	.reserved.max		= INIT_MEMBLOCK_REGIONS,

	.bottom_up		= false,
        // MEMBLOCK_ALLOC_ANYWHERE: 0xffffffff
	.current_limit		= MEMBLOCK_ALLOC_ANYWHERE,
};

// ARM10C 20131026
// KID 20140311
int memblock_debug __initdata_memblock;
// ARM10C 20131026
// KID 20140311
// memblock_can_resize: 1
static int memblock_can_resize __initdata_memblock;
static int memblock_memory_in_slab __initdata_memblock = 0;
static int memblock_reserved_in_slab __initdata_memblock = 0;

/* inline so we don't get a warning when pr_debug is compiled out */
static __init_memblock const char *
memblock_type_name(struct memblock_type *type)
{
	if (type == &memblock.memory)
		return "memory";
	else if (type == &memblock.reserved)
		return "reserved";
	else
		return "unknown";
}

/* adjust *@size so that (@base + *@size) doesn't overflow, return new size */
// ARM10C 20131019
// base: 0x20000000, size: 0x2f800000
// KID 20140307
// base: 0x20000000, size: 0x2f800000
// KID 20140311
// base: 0x20008000, size: 0x0051ED20
static inline phys_addr_t memblock_cap_size(phys_addr_t base, phys_addr_t *size)
{
	// *size: 0x2f800000, ULLONG_MAX: 0xFFFFFFFFFFFFFFFF, base: 0x20000000
	// *size: 0x0051ED20, ULLONG_MAX: 0xFFFFFFFFFFFFFFFF, base: 0x20008000
	return *size = min(*size, (phys_addr_t)ULLONG_MAX - base);
	// *size: 0x2f800000
	// *size: 0x0051ED20
}

/*
 * Address comparison utilities
 */
static unsigned long __init_memblock memblock_addrs_overlap(phys_addr_t base1, phys_addr_t size1,
				       phys_addr_t base2, phys_addr_t size2)
{
	return ((base1 < (base2 + size2)) && (base2 < (base1 + size1)));
}

static long __init_memblock memblock_overlaps_region(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size)
{
	unsigned long i;

	for (i = 0; i < type->cnt; i++) {
		phys_addr_t rgnbase = type->regions[i].base;
		phys_addr_t rgnsize = type->regions[i].size;
		if (memblock_addrs_overlap(base, size, rgnbase, rgnsize))
			break;
	}

	return (i < type->cnt) ? i : -1;
}

/*
 * __memblock_find_range_bottom_up - find free area utility in bottom-up
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_{ANYWHERE|ACCESSIBLE}
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %MAX_NUMNODES for any node
 *
 * Utility called from memblock_find_in_range_node(), find free area bottom-up.
 *
 * RETURNS:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock
__memblock_find_range_bottom_up(phys_addr_t start, phys_addr_t end,
				phys_addr_t size, phys_addr_t align, int nid)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	for_each_free_mem_range(i, nid, &this_start, &this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		cand = round_up(this_start, align);
		if (cand < this_end && this_end - cand >= size)
			return cand;
	}

	return 0;
}

/**
 * __memblock_find_range_top_down - find free area utility, in top-down
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_{ANYWHERE|ACCESSIBLE}
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %MAX_NUMNODES for any node
 *
 * Utility called from memblock_find_in_range_node(), find free area top-down.
 *
 * RETURNS:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock
__memblock_find_range_top_down(phys_addr_t start, phys_addr_t end,
			       phys_addr_t size, phys_addr_t align, int nid)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	for_each_free_mem_range_reverse(i, nid, &this_start, &this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		if (this_end < size)
			continue;

		cand = round_down(this_end - size, align);
		if (cand >= this_start)
			return cand;
	}

	return 0;
}

/**
 * memblock_find_in_range_node - find free area in given range and node
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_{ANYWHERE|ACCESSIBLE}
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %MAX_NUMNODES for any node
 *
 * Find @size free area aligned to @align in the specified range and node.
 *
 * When allocation direction is bottom-up, the @start should be greater
 * than the end of the kernel image. Otherwise, it will be trimmed. The
 * reason is that we want the bottom-up allocation just near the kernel
 * image so it is highly likely that the allocated memory and the kernel
 * will reside in the same node.
 *
 * If bottom-up allocation failed, will try to allocate memory top-down.
 *
 * RETURNS:
 * Found address on success, 0 on failure.
 */
// ARM10C 20131109
// 0, max_addr: 0, size: 0x00002000, align: 0x00002000, nid: 1
// lowmem 위에서 요청받은 size만큼 빈 영역을 찾아 시작주소 반환
phys_addr_t __init_memblock memblock_find_in_range_node(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align, int nid)
{
	int ret;
	phys_addr_t kernel_end;

	/* pump up @end */
	// end: 0x0, MEMBLOCK_ALLOC_ACCESSIBLE: 0
	if (end == MEMBLOCK_ALLOC_ACCESSIBLE)
		// end: 0x4f800000
		end = memblock.current_limit;

	/* avoid allocating the first page */
	// start: 0, PAGE_SIZE: 0x00001000
	// start: 0x00001000
	start = max_t(phys_addr_t, start, PAGE_SIZE);

	// end: 0x4f800000
	end = max(start, end);
	kernel_end = __pa_symbol(_end);

	/*
	 * try bottom-up allocation only when bottom-up mode
	 * is set and @end is above the kernel image.
	 */
	if (memblock_bottom_up() && end > kernel_end) {
		phys_addr_t bottom_up_start;

		/* make sure we will allocate above the kernel */
		bottom_up_start = max(start, kernel_end);

		/* ok, try bottom-up allocation first */
		ret = __memblock_find_range_bottom_up(bottom_up_start, end,
						      size, align, nid);
		if (ret)
			return ret;

		/*
		 * we always limit bottom-up allocation above the kernel,
		 * but top-down allocation doesn't have the limit, so
		 * retrying top-down allocation may succeed when bottom-up
		 * allocation failed.
		 *
		 * bottom-up allocation is expected to be fail very rarely,
		 * so we use WARN_ONCE() here to see the stack trace if
		 * fail happens.
		 */
		WARN_ONCE(1, "memblock: bottom-up allocation failed, "
			     "memory hotunplug may be affected\n");
	}

	return __memblock_find_range_top_down(start, end, size, align, nid);
}

/**
 * memblock_find_in_range - find free area in given range
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_{ANYWHERE|ACCESSIBLE}
 * @size: size of free area to find
 * @align: alignment of free area to find
 *
 * Find @size free area aligned to @align in the specified range.
 *
 * RETURNS:
 * Found address on success, 0 on failure.
 */
phys_addr_t __init_memblock memblock_find_in_range(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align)
{
	return memblock_find_in_range_node(start, end, size, align,
					   MAX_NUMNODES);
}

static void __init_memblock memblock_remove_region(struct memblock_type *type, unsigned long r)
{
	type->total_size -= type->regions[r].size;
	memmove(&type->regions[r], &type->regions[r + 1],
		(type->cnt - (r + 1)) * sizeof(type->regions[r]));
	type->cnt--;

	/* Special case for empty arrays */
	if (type->cnt == 0) {
		WARN_ON(type->total_size != 0);
		type->cnt = 1;
		type->regions[0].base = 0;
		type->regions[0].size = 0;
		memblock_set_region_node(&type->regions[0], MAX_NUMNODES);
	}
}

phys_addr_t __init_memblock get_allocated_memblock_reserved_regions_info(
					phys_addr_t *addr)
{
	if (memblock.reserved.regions == memblock_reserved_init_regions)
		return 0;

	*addr = __pa(memblock.reserved.regions);

	return PAGE_ALIGN(sizeof(struct memblock_region) *
			  memblock.reserved.max);
}

/**
 * memblock_double_array - double the size of the memblock regions array
 * @type: memblock type of the regions array being doubled
 * @new_area_start: starting address of memory range to avoid overlap with
 * @new_area_size: size of memory range to avoid overlap with
 *
 * Double the size of the @type regions array. If memblock is being used to
 * allocate memory for a new reserved regions array and there is a previously
 * allocated memory range [@new_area_start,@new_area_start+@new_area_size]
 * waiting to be reserved, ensure the memory used by the new array does
 * not overlap.
 *
 * RETURNS:
 * 0 on success, -1 on failure.
 */
static int __init_memblock memblock_double_array(struct memblock_type *type,
						phys_addr_t new_area_start,
						phys_addr_t new_area_size)
{
	struct memblock_region *new_array, *old_array;
	phys_addr_t old_alloc_size, new_alloc_size;
	phys_addr_t old_size, new_size, addr;
	int use_slab = slab_is_available();
	int *in_slab;

	/* We don't allow resizing until we know about the reserved regions
	 * of memory that aren't suitable for allocation
	 */
	if (!memblock_can_resize)
		return -1;

	/* Calculate new doubled size */
	old_size = type->max * sizeof(struct memblock_region);
	new_size = old_size << 1;
	/*
	 * We need to allocated new one align to PAGE_SIZE,
	 *   so we can free them completely later.
	 */
	old_alloc_size = PAGE_ALIGN(old_size);
	new_alloc_size = PAGE_ALIGN(new_size);

	/* Retrieve the slab flag */
	if (type == &memblock.memory)
		in_slab = &memblock_memory_in_slab;
	else
		in_slab = &memblock_reserved_in_slab;

	/* Try to find some space for it.
	 *
	 * WARNING: We assume that either slab_is_available() and we use it or
	 * we use MEMBLOCK for allocations. That means that this is unsafe to
	 * use when bootmem is currently active (unless bootmem itself is
	 * implemented on top of MEMBLOCK which isn't the case yet)
	 *
	 * This should however not be an issue for now, as we currently only
	 * call into MEMBLOCK while it's still active, or much later when slab
	 * is active for memory hotplug operations
	 */
	if (use_slab) {
		new_array = kmalloc(new_size, GFP_KERNEL);
		addr = new_array ? __pa(new_array) : 0;
	} else {
		/* only exclude range when trying to double reserved.regions */
		if (type != &memblock.reserved)
			new_area_start = new_area_size = 0;

		addr = memblock_find_in_range(new_area_start + new_area_size,
						memblock.current_limit,
						new_alloc_size, PAGE_SIZE);
		if (!addr && new_area_size)
			addr = memblock_find_in_range(0,
				min(new_area_start, memblock.current_limit),
				new_alloc_size, PAGE_SIZE);

		new_array = addr ? __va(addr) : NULL;
	}
	if (!addr) {
		pr_err("memblock: Failed to double %s array from %ld to %ld entries !\n",
		       memblock_type_name(type), type->max, type->max * 2);
		return -1;
	}

	memblock_dbg("memblock: %s is doubled to %ld at [%#010llx-%#010llx]",
			memblock_type_name(type), type->max * 2, (u64)addr,
			(u64)addr + new_size - 1);

	/*
	 * Found space, we now need to move the array over before we add the
	 * reserved region since it may be our reserved array itself that is
	 * full.
	 */
	memcpy(new_array, type->regions, old_size);
	memset(new_array + type->max, 0, old_size);
	old_array = type->regions;
	type->regions = new_array;
	type->max <<= 1;

	/* Free old array. We needn't free it if the array is the static one */
	if (*in_slab)
		kfree(old_array);
	else if (old_array != memblock_memory_init_regions &&
		 old_array != memblock_reserved_init_regions)
		memblock_free(__pa(old_array), old_alloc_size);

	/*
	 * Reserve the new array if that comes from the memblock.  Otherwise, we
	 * needn't do it
	 */
	if (!use_slab)
		BUG_ON(memblock_reserve(addr, new_alloc_size));

	/* Update slab flag */
	*in_slab = use_slab;

	return 0;
}

/**
 * memblock_merge_regions - merge neighboring compatible regions
 * @type: memblock type to scan
 *
 * Scan @type and merge neighboring compatible regions.
 */
// ARM10C 20131026
// KID 20140307
// [repeat] type: &memblock.memory
// KID 20140311
// [reserved][2nd] [repeat] type: &memblock.reserved
static void __init_memblock memblock_merge_regions(struct memblock_type *type)
{
	int i = 0;

	/* cnt never goes below 1 */
	// i: 0, type->cnt: memblock.memory.cnt: 2
	// i: 0, type->cnt: memblock.reserved.cnt: 2
	while (i < type->cnt - 1) {
		// i:0, &type->regions[0]: &memblock.memory.regions[0]
		// i:0, &type->regions[0]: &memblock.reserved.regions[0]
		struct memblock_region *this = &type->regions[i];
		// this: &memblock.memory.regions[0]
		// memblock.memory.regions[0].base: 0x20000000
		// memblock.memory.regions[0].size: 0x2f800000
		// this: &memblock.reserved.regions[0]
		// memblock.reserved.regions[0].base: 0x20004000
		// memblock.reserved.regions[0].size: 0x4000

		// i:0, &type->regions[1]: &memblock.memory.regions[1]
		// i:0, &type->regions[1]: &memblock.reserved.regions[1]
		struct memblock_region *next = &type->regions[i + 1];
		// next: &memblock.memory.regions[1]
		// memblock.memory.regions[1].base: 0x4f800000
		// memblock.memory.regions[1].size: 0x50800000
		// next: &memblock.reserved.regions[1]
		// memblock.reserved.regions[1].base: 0x20008000
		// memblock.reserved.regions[1].size: 0x0051ED20

		// this->base: 0x20000000, this->size: 0x2f800000, next->base: 0x4f800000
		// this->base + this->size: 0x4f800000
		// this->base: 0x20004000, this->size: 0x4000, next->base: 0x20008000
		// this->base + this->size: 0x20008000
		if (this->base + this->size != next->base ||
		    memblock_get_region_node(this) !=
		    memblock_get_region_node(next)) {
			BUG_ON(this->base + this->size > next->base);
			i++;
			continue;
		}

		// this->size: 0x2f800000, next->size: 0x50800000
		// this->size: 0x4000, next->size: 0x0051ED20
		this->size += next->size;
		// this->size: 0x80000000
		// this->size: 0x00522D20

		/* move forward from next + 1, index of which is i + 2 */
		// next: &memblock.memory.regions[1], next+1: &memblock.memory.regions[2]
		// type->cnt: memblock.memory.cnt: 2, i: 0,
		// type->cnt - (i + 2): 0, sizeof(*next): 8
		// next: &memblock.reserved.regions[1], next+1: &memblock.reserved.regions[2]
		// type->cnt: memblock.reserved.cnt: 2, i: 0,
		// type->cnt - (i + 2): 0, sizeof(*next): 8
		memmove(next, next + 1, (type->cnt - (i + 2)) * sizeof(*next));

		// type->cnt: memblock.memory.cnt: 2
		// type->cnt: memblock.reserved.cnt: 2
		type->cnt--;
		// type->cnt: memblock.memory.cnt: 1
		// type->cnt: memblock.reserved.cnt: 1
	}
}

/**
 * memblock_insert_region - insert new memblock region
 * @type:	memblock type to insert into
 * @idx:	index for the insertion point
 * @base:	base address of the new region
 * @size:	size of the new region
 * @nid:	node id of the new region
 *
 * Insert new memblock region [@base,@base+@size) into @type at @idx.
 * @type must already have extra room to accomodate the new region.
 */
// ARM10C 20131026
// i: 1, base: 0x4f800000, end - base: 0x50800000, nid: 1
// KID 20140307
// type: &memblock.memory, i: 1, base: 0x4f800000, end - base: 0x50800000, nid: 1
// KID 20140311
// type: &memblock.reserved, i: 0, base: 0x20004000, end - base: 0x4000, nid: 1
static void __init_memblock memblock_insert_region(struct memblock_type *type,
						   int idx, phys_addr_t base,
						   phys_addr_t size, int nid)
{
	// idx: 1, type->regions[1]: memblock.memory.regions[1]
	// idx: 0, type->regions[0]: memblock.reserved.regions[0]
	struct memblock_region *rgn = &type->regions[idx];
	// rgn: &memblock.memory.regions[1]
	// rgn: &memblock.reserved.regions[0]

	// type->cnt: memblock.memory.cnt: 1, type->max: memblock.memory.max: 128
	// type->cnt: memblock.reserved.cnt: 1, type->max: memblock.reserved.max: 128
	BUG_ON(type->cnt >= type->max);

	// rgn+1: &memblock.memory.regions[2], rgn: &memblock.memory.regions[1],
	// type->cnt: memblock.memory.cnt: 1, idx: 1, sizeof(*rgn): 8
	// rgn+1: &memblock.reserved.regions[1], rgn: &memblock.reserved.regions[0],
	// type->cnt: memblock.reserved.cnt: 1, idx: 0, sizeof(*rgn): 8
	memmove(rgn + 1, rgn, (type->cnt - idx) * sizeof(*rgn));

	// base: 0x4f800000
	// base: 0x20004000
	rgn->base = base;
	// rgn->base: memblock.memory.regions[1].base: 0x4f800000
	// rgn->base: memblock.reserved.regions[0].base: 0x20004000

	// size: 0x50800000
	// size: 0x4000
	rgn->size = size;
	// rgn->size: memblock.memory.regions[1].size: 0x50800000
	// rgn->size: memblock.reserved.regions[0].size: 0x4000

	// rgn: &memblock.memory.regions[1], nid: 1
	// rgn: &memblock.reserved.regions[0], nid: 1
	memblock_set_region_node(rgn, nid);

	// type->cnt: memblock.memory.cnt: 1
	// type->cnt: memblock.reserved.cnt: 1
	type->cnt++;
	// type->cnt: memblock.memory.cnt: 2
	// type->cnt: memblock.reserved.cnt: 2

	// type->total_size: memblock.memory.total_size: 0x2f800000, size: 0x50800000
	// type->total_size: memblock.reserved.total_size: 0x0051ED20, size: 0x4000
	type->total_size += size;
	// type->total_size: memblock.memory.total_size: 0x80000000
	// type->total_size: memblock.reserved.total_size: 0x00522D20
}

/**
 * memblock_add_region - add new memblock region
 * @type: memblock type to add new region into
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 *
 * Add new memblock region [@base,@base+@size) into @type.  The new region
 * is allowed to overlap with existing ones - overlaps don't affect already
 * existing regions.  @type is guaranteed to be minimal (all neighbouring
 * compatible regions are merged) after the addition.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
// ARM10C 20131019
// base: 0x20000000, size: 0x2f800000, nid: 1
// base: 0x4f800000, size: 0x50800000, nid: 1
// KID 20140307
// &memblock.memory, base: 0x20000000, size: 0x2f800000, MAX_NUMNODES: 1
// &memblock.memory, base: 0x4f800000, size: 0x50800000, MAX_NUMNODES: 1
// KID 20140311
// _rgn: &memblock.reserved, base: 0x20008000, size: 0x0051ED20, MAX_NUMNODES: 1
// _rgn: &memblock.reserved, base: 0x20004000, size: 0x4000, MAX_NUMNODES: 1
static int __init_memblock memblock_add_region(struct memblock_type *type,
				phys_addr_t base, phys_addr_t size, int nid)
{
	bool insert = false;
	// insert: 0

	// [memory][1st] base: 0x20000000
	// [memory][2nd] base: 0x4f800000
	// [reserved][1st] base: 0x20008000
	// [reserved][2nd] base: 0x20004000
	phys_addr_t obase = base;
	// [memory][1st] obase: 0x20000000
	// [memory][2nd] obase: 0x4f800000
	// [reserved][1st] obase: 0x20008000
	// [reserved][2nd] obase: 0x20004000

	// [memory][1st] base: 0x20000000, size: 0x2f800000
	// [memory][2nd] base: 0x4f800000, size: 0x50800000
	// [reserved][1st] base: 0x20008000, size: 0x0051ED20
	// [reserved][2nd] base: 0x20004000, size: 0x4000
	phys_addr_t end = base + memblock_cap_size(base, &size);
	// [memory][1st] end: 0x20000000 + 0x2f800000: 0x4f800000
	// [memory][2nd] end: 0x4f800000 + 0x50800000: 0xA0000000
	// [reserved][1st] end: 0x20008000 + 0x0051ED20: 0x20526D20
	// [reserved][2nd] end: 0x20004000 + 0x4000: 0x20008000
	int i, nr_new;

	if (!size)
		return 0;

	/* special case for empty array */
	// type: &memblock.memory
	// 2번째 call에선 size값이 0이 아님
	// [memory][1st] type->regions[0].size: memblock.memory.regions[0].size: 0
	// [memory][2nd] type->regions[0].size: memblock.memory.regions[0].size: 0x2f800000
	// [reserved][1st] type->regions[0].size: memblock.reserved.regions[0].size: 0
	// [reserved][2nd] type->regions[0].size: memblock.reserved.regions[0].size: 0x0051ED20
	if (type->regions[0].size == 0) {
		// [memory][1st] type->cnt: memblock.memory.regions[0].cnt: 1
		// [memory][1st] type->total_size: memblock.memory.regions[0].total_size: 0
		// [reserved][1st] type->cnt: memblock.reserved.regions[0].cnt: 1
		// [reserved][1st] type->total_size: memblock.reserved.regions[0].total_size: 0
		WARN_ON(type->cnt != 1 || type->total_size);

		// [memory][1st] base: 0x20000000
		// [reserved][1st] base: 0x20008000
		type->regions[0].base = base;
		// [memory][1st] type->regions[0].base: memblock.memory.regions[0].base: 0x20000000
		// [reserved][1st] type->regions[0].base: memblock.reserved.regions[0].base: 0x20008000

		// [memory][1st] size: 0x2f800000
		// [reserved][1st] size: 0x0051ED20
		type->regions[0].size = size;
		// [memory][1st] type->regions[0].size: memblock.memory.regions[0].size: 0x2f800000
		// [reserved][1st] type->regions[0].size: memblock.reserved.regions[0].size: 0x0051ED20

		// [memory][1st] type->regions[0]: memblock.memory.regions[0], nid: 1
		// [reserved][1st] type->regions[0]: memblock.reserved.regions[0], nid: 1
		memblock_set_region_node(&type->regions[0], nid);

		// [memory][1st] size: 0x2f800000
		// [reserved][1st] size: 0x0051ED20
		type->total_size = size;
		// [memory][1st] type->total_size: memblock.memory.total_size: 0x2f800000
		// [reserved][1st] type->total_size: memblock.reserved.total_size: 0x0051ED20

		return 0;
	}
repeat:
	/*
	 * The following is executed twice.  Once with %false @insert and
	 * then with %true.  The first counts the number of regions needed
	 * to accomodate the new area.  The second actually inserts them.
	 */
	// [memory][2nd] base: 0x4f800000, obase: 0x4f800000
	// [reserved][2nd] base: 0x20004000, obase: 0x20004000
	base = obase;
	// [memory][2nd] base: 0x4f800000
	// [reserved][2nd] base: 0x20004000

	nr_new = 0;
	// [memory][2nd] nr_new: 0
	// [reserved][2nd] nr_new: 0

	// [memory][2nd] type: &memblock.memory, type->cnt: memblock.memory.cnt: 1
	// [reserved][2nd] type: &memblock.reserved, type->cnt: memblock.reserved.cnt: 1
	for (i = 0; i < type->cnt; i++) {
		// [memory][2nd] i: 0, type->regions[0]: memblock.memory.regions[0]
		// [reserved][2nd] i: 0, type->regions[0]: memblock.reserved.regions[0]
		struct memblock_region *rgn = &type->regions[i];
		// [memory][2nd] rgn: &memblock.memory.regions[0]
		// [reserved][2nd] rgn: &memblock.reserved.regions[0]

		// [memory][2nd] rgn->base: memblock.memory.regions[0].base: 0x20000000
		// [reserved][2nd] rgn->base: memblock.reserved.regions[0].base: 0x20008000
		phys_addr_t rbase = rgn->base;
		// [memory][2nd] rbase: 0x20000000
		// [reserved][2nd] rbase: 0x20008000

		// [memory][2nd] rgn->size: memblock.memory.regions[0].size: 0x2f800000
		// [reserved][2nd] rgn->size: memblock.reserved.regions[0].size: 0x0051ED20
		phys_addr_t rend = rbase + rgn->size;
		// [memory][2nd] rend: 0x4f800000
		// [reserved][2nd] rend: 0x20526D20

		// [memory][2nd] rbase: 0x20000000, end: 0xA0000000
		// [reserved][2nd] rbase: 0x20008000, end: 0x20008000
		if (rbase >= end)
			break;
			// [reserved][2nd] 루프 빠져 나감

		// [memory][2nd] rend: 0x4f800000, base: 0x4f800000
		if (rend <= base)
			continue;
			// [memory][2nd] 루프 빠져 나감
		/*
		 * @rgn overlaps.  If it separates the lower part of new
		 * area, insert that portion.
		 */
		if (rbase > base) {
			nr_new++;
			if (insert)
				memblock_insert_region(type, i++, base,
						       rbase - base, nid);
		}
		/* area below @rend is dealt with, forget about it */
		base = min(rend, end);
	}

	/* insert the remaining portion */
	// [memory][2nd] base: 0x4f800000, end: 0xA0000000
	// [reserved][2nd] base: 0x20004000, end: 0x20008000
	if (base < end) {
		// [memory][2nd] nr_new: 0
		// [reserved][2nd] nr_new: 0
		nr_new++;
		// [memory][2nd] nr_new: 1
		// [reserved][2nd] nr_new: 1

		// [memory][2nd] insert: 0
		// [memory][2nd] [repeat] insert: 1
		// [reserved][2nd] insert: 0
		// [reserved][2nd] [repeat] insert: 1
		if (insert)
			// [memory][2nd] repeat로 jump후 들어옴
			// [memory][2nd] type: &memblock.memory
			// [memory][2nd] i: 1, base: 0x4f800000, end - base: 0x50800000, nid: 1
			// [reserved][2nd] repeat로 jump후 들어옴
			// [reserved][2nd] type: &memblock.reserved
			// [reserved][2nd] i: 0, base: 0x20004000, end - base: 0x4000, nid: 1
			memblock_insert_region(type, i, base, end - base, nid);
	}

	/*
	 * If this was the first round, resize array and repeat for actual
	 * insertions; otherwise, merge and return.
	 */
	// [memory][2nd] insert: 0
	// [memory][2nd] [repeat] insert: 1
	// [reserved][2nd] insert: 0
	// [reserved][2nd] [repeat] insert: 1
	if (!insert) {
		// [memory][2nd] type->cnt: memblock.memory.cnt: 1, nr_new: 1,
		// [memory][2nd] type->max: memblock.memory.max: 128
		// [reserved][2nd] type->cnt: memblock.reserved.cnt: 1, nr_new: 1,
		// [reserved][2nd] type->max: memblock.reserved.max: 128
		while (type->cnt + nr_new > type->max)
			if (memblock_double_array(type, obase, size) < 0)
				return -ENOMEM;
		insert = true;
		// [memory][2nd] insert: 1
		// [reserved][2nd] insert: 1

		goto repeat;
	} else {
		// [memory][2nd] [repeat] type: &memblock.memory
		// [reserved][2nd] [repeat] type: &memblock.reserved
		memblock_merge_regions(type);
		// [memory][2nd] 연속된 메모리 영역을 합침.
		// [memory][2nd] 우리도 0x20000000~0x2f800000, 0x4f800000~0x50800000 이므로 합쳐짐.
		// [memory][2nd] 합쳐진 영역 0x20000000~0xA0000000 으로 regions 1개로 변경됨
		// [reserved][2nd] 연속된 메모리 영역을 합침.
		// [reserved][2nd] 우리도 0x20004000~0x4000, 0x20008000~0x0051ED20 이므로 합쳐짐.
		// [reserved][2nd] 합쳐진 영역 0x20004000~0x20526D20 으로 regions 1개로 변경됨

		return 0;
	}
}

int __init_memblock memblock_add_node(phys_addr_t base, phys_addr_t size,
				       int nid)
{
	return memblock_add_region(&memblock.memory, base, size, nid);
}

// ARM10C 20131019
// base: 0x20000000, size: 0x2f800000
// base: 0x4f800000, size: 0x50800000
// KID 20140307
// i: 0, mi->bank[0].start: 0x20000000, mi->bank[0].size: 0x2f800000
// i: 1, mi->bank[1].start: 0x4f800000, mi->bank[1].size: 0x50800000
int __init_memblock memblock_add(phys_addr_t base, phys_addr_t size)
{
	// base: 0x20000000, size: 0x2f800000, MAX_NUMNODES: 1
	// base: 0x4f800000, size: 0x50800000, MAX_NUMNODES: 1
	return memblock_add_region(&memblock.memory, base, size, MAX_NUMNODES);
	// return 0
	// return 0
}

/**
 * memblock_isolate_range - isolate given range into disjoint memblocks
 * @type: memblock type to isolate range for
 * @base: base of range to isolate
 * @size: size of range to isolate
 * @start_rgn: out parameter for the start of isolated region
 * @end_rgn: out parameter for the end of isolated region
 *
 * Walk @type and ensure that regions don't cross the boundaries defined by
 * [@base,@base+@size).  Crossing regions are split at the boundaries,
 * which may create at most two more regions.  The index of the first
 * region inside the range is returned in *@start_rgn and end in *@end_rgn.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
static int __init_memblock memblock_isolate_range(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size,
					int *start_rgn, int *end_rgn)
{
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int i;

	*start_rgn = *end_rgn = 0;

	if (!size)
		return 0;

	/* we'll create at most two more regions */
	while (type->cnt + 2 > type->max)
		if (memblock_double_array(type, base, size) < 0)
			return -ENOMEM;

	for (i = 0; i < type->cnt; i++) {
		struct memblock_region *rgn = &type->regions[i];
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		if (rbase >= end)
			break;
		if (rend <= base)
			continue;

		if (rbase < base) {
			/*
			 * @rgn intersects from below.  Split and continue
			 * to process the next region - the new top half.
			 */
			rgn->base = base;
			rgn->size -= base - rbase;
			type->total_size -= base - rbase;
			memblock_insert_region(type, i, rbase, base - rbase,
					       memblock_get_region_node(rgn));
		} else if (rend > end) {
			/*
			 * @rgn intersects from above.  Split and redo the
			 * current region - the new bottom half.
			 */
			rgn->base = end;
			rgn->size -= end - rbase;
			type->total_size -= end - rbase;
			memblock_insert_region(type, i--, rbase, end - rbase,
					       memblock_get_region_node(rgn));
		} else {
			/* @rgn is fully contained, record it */
			if (!*end_rgn)
				*start_rgn = i;
			*end_rgn = i + 1;
		}
	}

	return 0;
}

static int __init_memblock __memblock_remove(struct memblock_type *type,
					     phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = end_rgn - 1; i >= start_rgn; i--)
		memblock_remove_region(type, i);
	return 0;
}

int __init_memblock memblock_remove(phys_addr_t base, phys_addr_t size)
{
	return __memblock_remove(&memblock.memory, base, size);
}

int __init_memblock memblock_free(phys_addr_t base, phys_addr_t size)
{
	memblock_dbg("   memblock_free: [%#016llx-%#016llx] %pF\n",
		     (unsigned long long)base,
		     (unsigned long long)base + size,
		     (void *)_RET_IP_);

	return __memblock_remove(&memblock.reserved, base, size);
}

// ARM10C 20131026
// base: 0x20008000
// base부터 size만큼 memblock.reserved에 등록한다.
// KID 20140311
// __pa(_stext): 0x20008000, _end - _stext: 0x0051ED20
// __pa(swapper_pg_dir); 0x20004000, SWAPPER_PG_DIR_SIZE: 0x4000
// virt_to_phys(initial_boot_params): atag / devtree의 위치 하고 있는 물리 주소 값,
// be32_to_cpu(initial_boot_params->totalsize): 0x3236
int __init_memblock memblock_reserve(phys_addr_t base, phys_addr_t size)
{
	struct memblock_type *_rgn = &memblock.reserved;
	// _rgn: &memblock.reserved

	// base: 0x20008000, size: 0x0051ED20
	// base: 0x20004000, size: 0x4000
	memblock_dbg("memblock_reserve: [%#016llx-%#016llx] %pF\n",
		     (unsigned long long)base,
		     (unsigned long long)base + size,
		     (void *)_RET_IP_);
	// 메모리 region의 reserved 영역을 출력

	// _rgn: &memblock.reserved, base: 0x20008000, size: 0x0051ED20, MAX_NUMNODES: 1
	// _rgn: &memblock.reserved, base: 0x20004000, size: 0x4000, MAX_NUMNODES: 1
	return memblock_add_region(_rgn, base, size, MAX_NUMNODES);
}

/**
 * __next_free_mem_range - next function for for_each_free_mem_range()
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %MAX_NUMNODES for all nodes
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Find the first free area from *@idx which matches @nid, fill the out
 * parameters, and update *@idx for the next iteration.  The lower 32bit of
 * *@idx contains index into memory region and the upper 32bit indexes the
 * areas before each reserved region.  For example, if reserved regions
 * look like the following,
 *
 *	0:[0-16), 1:[32-48), 2:[128-130)
 *
 * The upper 32bit indexes the following regions.
 *
 *	0:[0-0), 1:[16-32), 2:[48-128), 3:[130-MAX)
 *
 * As both region arrays are sorted, the function advances the two indices
 * in lockstep and returns each intersection.
 */
void __init_memblock __next_free_mem_range(u64 *idx, int nid,
					   phys_addr_t *out_start,
					   phys_addr_t *out_end, int *out_nid)
{
	struct memblock_type *mem = &memblock.memory;
	struct memblock_type *rsv = &memblock.reserved;
	int mi = *idx & 0xffffffff;
	int ri = *idx >> 32;

	for ( ; mi < mem->cnt; mi++) {
		struct memblock_region *m = &mem->regions[mi];
		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;

		/* only memory regions are associated with nodes, check it */
		if (nid != MAX_NUMNODES && nid != memblock_get_region_node(m))
			continue;

		/* scan areas before each reservation for intersection */
		for ( ; ri < rsv->cnt + 1; ri++) {
			struct memblock_region *r = &rsv->regions[ri];
			phys_addr_t r_start = ri ? r[-1].base + r[-1].size : 0;
			phys_addr_t r_end = ri < rsv->cnt ? r->base : ULLONG_MAX;

			/* if ri advanced past mi, break out to advance mi */
			if (r_start >= m_end)
				break;
			/* if the two regions intersect, we're done */
			if (m_start < r_end) {
				if (out_start)
					*out_start = max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = memblock_get_region_node(m);
				/*
				 * The region which ends first is advanced
				 * for the next iteration.
				 */
				if (m_end <= r_end)
					mi++;
				else
					ri++;
				*idx = (u32)mi | (u64)ri << 32;
				return;
			}
		}
	}

	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/**
 * __next_free_mem_range_rev - next function for for_each_free_mem_range_reverse()
 * @idx: pointer to u64 loop variable
 * @nid: nid: node selector, %MAX_NUMNODES for all nodes
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Reverse of __next_free_mem_range().
 */
// ARM10C 20131109
// *idx: ULLONG_MAX, nid: 1
void __init_memblock __next_free_mem_range_rev(u64 *idx, int nid,
					   phys_addr_t *out_start,
					   phys_addr_t *out_end, int *out_nid)
{
	struct memblock_type *mem = &memblock.memory;
	struct memblock_type *rsv = &memblock.reserved;
	int mi = *idx & 0xffffffff;
	int ri = *idx >> 32;

	if (*idx == (u64)ULLONG_MAX) {
		// mem->cnt: 1, rsv->cnt: 5
		mi = mem->cnt - 1;
		ri = rsv->cnt;
	}

	for ( ; mi >= 0; mi--) {
		struct memblock_region *m = &mem->regions[mi];
		// m->base: 0x20000000
		phys_addr_t m_start = m->base;
		// m->size: 0x80000000, m_end: 0xA0000000
		phys_addr_t m_end = m->base + m->size;

		/* only memory regions are associated with nodes, check it */
		// nid: 1, MAX_NUMNODES: 1
		if (nid != MAX_NUMNODES && nid != memblock_get_region_node(m))
			continue;

		/* scan areas before each reservation for intersection */
		// ri: 5
		for ( ; ri >= 0; ri--) {
			// dtb reseved map 가정
			// base: 0x60000000, size: 0x00100000
			struct memblock_region *r = &rsv->regions[ri];
			// r[-1].base, r[-1].size: dtb reseved map
			// r_start: 0x60100000 
			phys_addr_t r_start = ri ? r[-1].base + r[-1].size : 0;
			// r_end: ULLONG_MAX
			phys_addr_t r_end = ri < rsv->cnt ? r->base : ULLONG_MAX;

			/* if ri advanced past mi, break out to advance mi */
			// r_end: ULLONG_MAX, m_start: 0x20000000
			if (r_end <= m_start)
				break;
			/* if the two regions intersect, we're done */
			// m_end: 0xA0000000, r_start: 0x60100000 
			if (m_end > r_start) {
				if (out_start)
					// m_start: 0x20000000, r_start: 0x60100000 
					// *out_start: 0x60100000
					*out_start = max(m_start, r_start);
				if (out_end)
					// m_end: 0xA0000000, r_end: ULLONG_MAX
					// *out_end: 0xA0000000
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = memblock_get_region_node(m);

				// m_start: 0x20000000, r_start: 0x60100000 
				if (m_start >= r_start)
					mi--;
				else
					// ri: 4
					ri--;
				// mi: 0, ri: 4
				*idx = (u32)mi | (u64)ri << 32;
				return;
			}
		}
	}

	*idx = ULLONG_MAX;
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
/*
 * Common iterator interface used to define for_each_mem_range().
 */
void __init_memblock __next_mem_pfn_range(int *idx, int nid,
				unsigned long *out_start_pfn,
				unsigned long *out_end_pfn, int *out_nid)
{
	struct memblock_type *type = &memblock.memory;
	struct memblock_region *r;

	while (++*idx < type->cnt) {
		r = &type->regions[*idx];

		if (PFN_UP(r->base) >= PFN_DOWN(r->base + r->size))
			continue;
		if (nid == MAX_NUMNODES || nid == r->nid)
			break;
	}
	if (*idx >= type->cnt) {
		*idx = -1;
		return;
	}

	if (out_start_pfn)
		*out_start_pfn = PFN_UP(r->base);
	if (out_end_pfn)
		*out_end_pfn = PFN_DOWN(r->base + r->size);
	if (out_nid)
		*out_nid = r->nid;
}

/**
 * memblock_set_node - set node ID on memblock regions
 * @base: base of area to set node ID for
 * @size: size of area to set node ID for
 * @nid: node ID to set
 *
 * Set the nid of memblock memory regions in [@base,@base+@size) to @nid.
 * Regions which cross the area boundaries are split as necessary.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_set_node(phys_addr_t base, phys_addr_t size,
				      int nid)
{
	struct memblock_type *type = &memblock.memory;
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++)
		memblock_set_region_node(&type->regions[i], nid);

	memblock_merge_regions(type);
	return 0;
}
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

// ARM10C 20131109
// size: 0x00002000, align: 0x00002000, MEMBLOCK_ALLOC_ACCESSIBLE: 0, MAX_NUMNODES: 1
static phys_addr_t __init memblock_alloc_base_nid(phys_addr_t size,
					phys_addr_t align, phys_addr_t max_addr,
					int nid)
{
	phys_addr_t found;

	// align: 0x00002000
	if (WARN_ON(!align))
		align = __alignof__(long long);

	/* align @size to avoid excessive fragmentation on reserved array */
	// size: 0x00002000
	size = round_up(size, align);

	// 0, max_addr: 0, size: 0x00002000, align: 0x00002000, nid: 1
	found = memblock_find_in_range_node(0, max_addr, size, align, nid);
	if (found && !memblock_reserve(found, size))
		return found;

	return 0;
}

// ARM10C 20131109
// size: 0x00002000, align: 0x00002000, max_addr: 0, MAX_NUMNODES: 1
phys_addr_t __init memblock_alloc_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	// size: 0x00002000, align: 0x00002000, MEMBLOCK_ALLOC_ACCESSIBLE: 0, MAX_NUMNODES: 1
	return memblock_alloc_base_nid(size, align, MEMBLOCK_ALLOC_ACCESSIBLE, nid);
}

// ARM10C 20131109
// size: 0x00002000, align: 0x00002000, max_addr: 0
phys_addr_t __init __memblock_alloc_base(phys_addr_t size, phys_addr_t align, phys_addr_t max_addr)
{
	// size: 0x00002000, align: 0x00002000, max_addr: 0, MAX_NUMNODES: 1
	return memblock_alloc_base_nid(size, align, max_addr, MAX_NUMNODES);
}

// ARM10C 20131109
// size: 0x00002000, align: 0x00002000, MEMBLOCK_ALLOC_ACCESSIBLE: 0
phys_addr_t __init memblock_alloc_base(phys_addr_t size, phys_addr_t align, phys_addr_t max_addr)
{
	phys_addr_t alloc;

	// size: 0x00002000, align: 0x00002000, max_addr: 0
	alloc = __memblock_alloc_base(size, align, max_addr);

	if (alloc == 0)
		panic("ERROR: Failed to allocate 0x%llx bytes below 0x%llx.\n",
		      (unsigned long long) size, (unsigned long long) max_addr);

	return alloc;
}

// ARM10C 20131109
// sz: 0x00002000, align: 0x00002000
phys_addr_t __init memblock_alloc(phys_addr_t size, phys_addr_t align)
{
	// size: 0x00002000, align: 0x00002000, MEMBLOCK_ALLOC_ACCESSIBLE: 0
	return memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
}

phys_addr_t __init memblock_alloc_try_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	phys_addr_t res = memblock_alloc_nid(size, align, nid);

	if (res)
		return res;
	return memblock_alloc_base(size, align, MEMBLOCK_ALLOC_ACCESSIBLE);
}


/*
 * Remaining API functions
 */

phys_addr_t __init memblock_phys_mem_size(void)
{
	return memblock.memory.total_size;
}

phys_addr_t __init memblock_mem_size(unsigned long limit_pfn)
{
	unsigned long pages = 0;
	struct memblock_region *r;
	unsigned long start_pfn, end_pfn;

	for_each_memblock(memory, r) {
		start_pfn = memblock_region_memory_base_pfn(r);
		end_pfn = memblock_region_memory_end_pfn(r);
		start_pfn = min_t(unsigned long, start_pfn, limit_pfn);
		end_pfn = min_t(unsigned long, end_pfn, limit_pfn);
		pages += end_pfn - start_pfn;
	}

	return (phys_addr_t)pages << PAGE_SHIFT;
}

/* lowest address */
phys_addr_t __init_memblock memblock_start_of_DRAM(void)
{
	return memblock.memory.regions[0].base;
}

phys_addr_t __init_memblock memblock_end_of_DRAM(void)
{
	int idx = memblock.memory.cnt - 1;

	return (memblock.memory.regions[idx].base + memblock.memory.regions[idx].size);
}

void __init memblock_enforce_memory_limit(phys_addr_t limit)
{
	unsigned long i;
	phys_addr_t max_addr = (phys_addr_t)ULLONG_MAX;

	if (!limit)
		return;

	/* find out max address */
	for (i = 0; i < memblock.memory.cnt; i++) {
		struct memblock_region *r = &memblock.memory.regions[i];

		if (limit <= r->size) {
			max_addr = r->base + limit;
			break;
		}
		limit -= r->size;
	}

	/* truncate both memory and reserved regions */
	__memblock_remove(&memblock.memory, max_addr, (phys_addr_t)ULLONG_MAX);
	__memblock_remove(&memblock.reserved, max_addr, (phys_addr_t)ULLONG_MAX);
}

// ARM10C 20131026
static int __init_memblock memblock_search(struct memblock_type *type, phys_addr_t addr)
{
	// type->cnt: 1
	unsigned int left = 0, right = type->cnt;

	do {
		// mid: 0
		unsigned int mid = (right + left) / 2;

		if (addr < type->regions[mid].base)
			right = mid;
		else if (addr >= (type->regions[mid].base +
				  type->regions[mid].size))
			left = mid + 1;
		else
			return mid;
	} while (left < right);
	return -1;
}

int __init memblock_is_reserved(phys_addr_t addr)
{
	return memblock_search(&memblock.reserved, addr) != -1;
}

// ARM10C 20140118
// __pfn_to_phys(pfn) : 0x20000000
int __init_memblock memblock_is_memory(phys_addr_t addr)
{
	// addr : 0x20000000
	return memblock_search(&memblock.memory, addr) != -1;
	// addr이 속한 region의 번호 0이 -1하고 다르니까 1 리턴 
}

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
int __init_memblock memblock_search_pfn_nid(unsigned long pfn,
			 unsigned long *start_pfn, unsigned long *end_pfn)
{
	struct memblock_type *type = &memblock.memory;
	int mid = memblock_search(type, (phys_addr_t)pfn << PAGE_SHIFT);

	if (mid == -1)
		return -1;

	*start_pfn = type->regions[mid].base >> PAGE_SHIFT;
	*end_pfn = (type->regions[mid].base + type->regions[mid].size)
			>> PAGE_SHIFT;

	return type->regions[mid].nid;
}
#endif

/**
 * memblock_is_region_memory - check if a region is a subset of memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base+@size) is a subset of a memory block.
 *
 * RETURNS:
 * 0 if false, non-zero if true
 */
// ARM10C 20131026
int __init_memblock memblock_is_region_memory(phys_addr_t base, phys_addr_t size)
{
	// idx: 0
	int idx = memblock_search(&memblock.memory, base);
	phys_addr_t end = base + memblock_cap_size(base, &size);

	if (idx == -1)
		return 0;
	return memblock.memory.regions[idx].base <= base &&
		(memblock.memory.regions[idx].base +
		 memblock.memory.regions[idx].size) >= end;
}

/**
 * memblock_is_region_reserved - check if a region intersects reserved memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base+@size) intersects a reserved memory block.
 *
 * RETURNS:
 * 0 if false, non-zero if true
 */
int __init_memblock memblock_is_region_reserved(phys_addr_t base, phys_addr_t size)
{
	memblock_cap_size(base, &size);
	return memblock_overlaps_region(&memblock.reserved, base, size) >= 0;
}

void __init_memblock memblock_trim_memory(phys_addr_t align)
{
	int i;
	phys_addr_t start, end, orig_start, orig_end;
	struct memblock_type *mem = &memblock.memory;

	for (i = 0; i < mem->cnt; i++) {
		orig_start = mem->regions[i].base;
		orig_end = mem->regions[i].base + mem->regions[i].size;
		start = round_up(orig_start, align);
		end = round_down(orig_end, align);

		if (start == orig_start && end == orig_end)
			continue;

		if (start < end) {
			mem->regions[i].base = start;
			mem->regions[i].size = end - start;
		} else {
			memblock_remove_region(mem, i);
			i--;
		}
	}
}

// ARM10C 20131019
// KID 20140307
// memblock_limit: 0x4f800000
void __init_memblock memblock_set_current_limit(phys_addr_t limit)
{
	// limit: 0x4f800000
	memblock.current_limit = limit;
	// memblock.current_limit: 0x4f800000
}

// ARM10C 20131026
// KID 20140311
// &memblock.memory, "memory"
// &memblock.reserved, "reserved"
static void __init_memblock memblock_dump(struct memblock_type *type, char *name)
{
	unsigned long long base, size;
	int i;

	// name: "memory", type->cnt: memblock.memory.cnt: 1
	pr_info(" %s.cnt  = 0x%lx\n", name, type->cnt);

	// type->cnt: memblock.memory.cnt: 1
	for (i = 0; i < type->cnt; i++) {
		// i: 0, type->regions[0]: memblock.memory.regions[0]
		struct memblock_region *rgn = &type->regions[i];
		// rgn: memblock.memory.regions[0]

		char nid_buf[32] = "";

		// rgn->base: memblock.memory.regions[0].base: 0x20000000
		base = rgn->base;
		// base: 0x20000000

		// rgn->size: memblock.memory.regions[0].size: 0x80000000
		size = rgn->size;
		// size: 0x80000000

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP // CONFIG_HAVE_MEMBLOCK_NODE_MAP=n
		if (memblock_get_region_node(rgn) != MAX_NUMNODES)
			snprintf(nid_buf, sizeof(nid_buf), " on node %d",
				 memblock_get_region_node(rgn));
#endif
		// name: "memory", i: 0, base: 0x20000000, size: 0x80000000
		// base + size - 1: 0x9FFFFFFF, nid_buf: ""
		pr_info(" %s[%#x]\t[%#016llx-%#016llx], %#llx bytes%s\n",
			name, i, base, base + size - 1, size, nid_buf);
	}
}

// ARM10C 20131026
// KID 20140311
void __init_memblock __memblock_dump_all(void)
{
	pr_info("MEMBLOCK configuration:\n");

	// memblock.memory.total_size: 0x80000000
	// memblock.reserved.total_size: 0x00522D20 + devtree 사이즈
	pr_info(" memory size = %#llx reserved size = %#llx\n",
		(unsigned long long)memblock.memory.total_size,
		(unsigned long long)memblock.reserved.total_size);

	memblock_dump(&memblock.memory, "memory");
	memblock_dump(&memblock.reserved, "reserved");
}

// ARM10C 20131026
// KID 20140311
void __init memblock_allow_resize(void)
{
	memblock_can_resize = 1;
}

// ARM10C 20131026
// KID 20140311
static int __init early_memblock(char *p)
{
	if (p && strstr(p, "debug"))
		memblock_debug = 1;
	return 0;
}
early_param("memblock", early_memblock);

#if defined(CONFIG_DEBUG_FS) && !defined(CONFIG_ARCH_DISCARD_MEMBLOCK)

static int memblock_debug_show(struct seq_file *m, void *private)
{
	struct memblock_type *type = m->private;
	struct memblock_region *reg;
	int i;

	for (i = 0; i < type->cnt; i++) {
		reg = &type->regions[i];
		seq_printf(m, "%4d: ", i);
		if (sizeof(phys_addr_t) == 4)
			seq_printf(m, "0x%08lx..0x%08lx\n",
				   (unsigned long)reg->base,
				   (unsigned long)(reg->base + reg->size - 1));
		else
			seq_printf(m, "0x%016llx..0x%016llx\n",
				   (unsigned long long)reg->base,
				   (unsigned long long)(reg->base + reg->size - 1));

	}
	return 0;
}

static int memblock_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, memblock_debug_show, inode->i_private);
}

static const struct file_operations memblock_debug_fops = {
	.open = memblock_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init memblock_init_debugfs(void)
{
	struct dentry *root = debugfs_create_dir("memblock", NULL);
	if (!root)
		return -ENXIO;
	debugfs_create_file("memory", S_IRUGO, root, &memblock.memory, &memblock_debug_fops);
	debugfs_create_file("reserved", S_IRUGO, root, &memblock.reserved, &memblock_debug_fops);

	return 0;
}
__initcall(memblock_init_debugfs);

#endif /* CONFIG_DEBUG_FS */
