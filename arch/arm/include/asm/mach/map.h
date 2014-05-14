/*
 *  arch/arm/include/asm/map.h
 *
 *  Copyright (C) 1999-2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Page table mapping constructs and function prototypes
 */
#ifndef __ASM_MACH_MAP_H
#define __ASM_MACH_MAP_H

#include <asm/io.h>

// ARM10C 20131102
// KID 20140418
struct map_desc {
	unsigned long virtual;
	unsigned long pfn;
	unsigned long length;
	unsigned int type;
};

/* types 0-3 are defined in asm/io.h */
// KID 20140321
// MT_DEVICE: 0, MT_DEVICE_NONSHARED: 1, MT_DEVICE_CACHED: 2, MT_DEVICE_WC: 3
#define MT_UNCACHED		4
// KID 20140321
#define MT_CACHECLEAN		5
#define MT_MINICLEAN		6
// KID 20140321
#define MT_LOW_VECTORS		7
// KID 20140321
#define MT_HIGH_VECTORS		8
// KID 20140321
// KID 20140418
#define MT_MEMORY		9
// KID 20140321
#define MT_ROM			10
// KID 20140321
#define MT_MEMORY_NONCACHED	11
// KID 20140321
#define MT_MEMORY_DTCM		12
// KID 20140321
#define MT_MEMORY_ITCM		13
// KID 20140321
#define MT_MEMORY_SO		14
#define MT_MEMORY_DMA_READY	15

#ifdef CONFIG_MMU // CONFIG_MMU=y
extern void iotable_init(struct map_desc *, int);
extern void vm_reserve_area_early(unsigned long addr, unsigned long size,
				  void *caller);

#ifdef CONFIG_DEBUG_LL // CONFIG_DEBUG_LL=n
extern void debug_ll_addr(unsigned long *paddr, unsigned long *vaddr);
extern void debug_ll_io_init(void);
#else
// ARM10C 20131116
static inline void debug_ll_io_init(void) {}
#endif

struct mem_type;
extern const struct mem_type *get_mem_type(unsigned int type);
/*
 * external interface to remap single page with appropriate type
 */
extern int ioremap_page(unsigned long virt, unsigned long phys,
			const struct mem_type *mtype);
#else
#define iotable_init(map,num)	do { } while (0)
#define vm_reserve_area_early(a,s,c)	do { } while (0)
#endif

#endif
