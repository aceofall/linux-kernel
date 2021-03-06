#ifndef _LINUX_SLUB_DEF_H
#define _LINUX_SLUB_DEF_H

/*
 * SLUB : A Slab allocator without object queues.
 *
 * (C) 2007 SGI, Christoph Lameter
 */
#include <linux/kobject.h>

// ARM10C 20140531
// ARM10C 20140621
// ARM10C 20140712
// ARM10C 20140719
enum stat_item {
	// ALLOC_FASTPATH: 0
	ALLOC_FASTPATH,		/* Allocation from cpu slab */
	ALLOC_SLOWPATH,		/* Allocation by getting a new cpu slab */
	// FREE_FASTPATH: 2
	FREE_FASTPATH,		/* Free to cpu slab */
	FREE_SLOWPATH,		/* Freeing not to cpu slab */
	FREE_FROZEN,		/* Freeing to frozen slab */
	FREE_ADD_PARTIAL,	/* Freeing moves slab to partial list */
	FREE_REMOVE_PARTIAL,	/* Freeing removes last object */
	// ALLOC_FROM_PARTIAL: 7
	ALLOC_FROM_PARTIAL,	/* Cpu slab acquired from node partial list */
	// ALLOC_SLAB: 8
	ALLOC_SLAB,		/* Cpu slab acquired from page allocator */
	ALLOC_REFILL,		/* Refill cpu slab from slab freelist */
	ALLOC_NODE_MISMATCH,	/* Switching cpu slab */
	FREE_SLAB,		/* Slab freed to the page allocator */
	// CPUSLAB_FLUSH: 13
	CPUSLAB_FLUSH,		/* Abandoning of the cpu slab */
	DEACTIVATE_FULL,	/* Cpu slab was full when deactivated */
	DEACTIVATE_EMPTY,	/* Cpu slab was empty when deactivated */
	// DEACTIVATE_TO_HEAD: 15
	DEACTIVATE_TO_HEAD,	/* Cpu slab was moved to the head of partials */
	// DEACTIVATE_TO_TAIL: 16
	DEACTIVATE_TO_TAIL,	/* Cpu slab was moved to the tail of partials */
	// DEACTIVATE_REMOTE_FREES: 17
	DEACTIVATE_REMOTE_FREES,/* Slab contained remotely freed objects */
	DEACTIVATE_BYPASS,	/* Implicit deactivation */
	ORDER_FALLBACK,		/* Number of times fallback was necessary */
	CMPXCHG_DOUBLE_CPU_FAIL,/* Failure of this_cpu_cmpxchg_double */
	CMPXCHG_DOUBLE_FAIL,	/* Number of times that cmpxchg double did not match */
	CPU_PARTIAL_ALLOC,	/* Used cpu partial on alloc */
	CPU_PARTIAL_FREE,	/* Refill cpu partial on free */
	CPU_PARTIAL_NODE,	/* Refill cpu partial from node partial */
	CPU_PARTIAL_DRAIN,	/* Drain cpu partial to node partial */
	NR_SLUB_STAT_ITEMS };

// ARM10C 20140531
// ARM10C 20140614
// ARM10C 20141206
// sizeof(struct kmem_cache_cpu): 16 bytes
struct kmem_cache_cpu {
	void **freelist;	/* Pointer to next available object */
	unsigned long tid;	/* Globally unique transaction id */
	struct page *page;	/* The slab from which we are allocating */
	struct page *partial;	/* Partially allocated frozen slabs */
#ifdef CONFIG_SLUB_STATS // CONFIG_SLUB_STATS=n
	unsigned stat[NR_SLUB_STAT_ITEMS];
#endif
};

/*
 * Word size structure that can be atomically updated or read and that
 * contains both the order and the number of objects that a slab of the
 * given order would contain.
 */
// ARM10C 20140419
struct kmem_cache_order_objects {
	unsigned long x;
};

/*
 * Slab cache management.
 */
// ARM10C 20140419
// ARM10C 20140607
// ARM10C 20141206
// ARM10C 20150912
// ARM10C 20150919
// ARM10C 20151003
// ARM10C 20151024
// ARM10C 20151031
// ARM10C 20160123
// ARM10C 20160521
// ARM10C 20160528
// ARM10C 20160604
// sizeof(struct kmem_cache): 116 bytes
struct kmem_cache {
	struct kmem_cache_cpu __percpu *cpu_slab;
	/* Used for retriving partial slabs etc */
	unsigned long flags;
	unsigned long min_partial;
	int size;		/* The size of an object including meta data */
	int object_size;	/* The size of an object without meta data */
	int offset;		/* Free pointer offset. */
	int cpu_partial;	/* Number of per cpu partial objects to keep around */
	struct kmem_cache_order_objects oo;

	/* Allocation and freeing of slabs */
	struct kmem_cache_order_objects max;
	struct kmem_cache_order_objects min;
	gfp_t allocflags;	/* gfp flags to use on each alloc */
	int refcount;		/* Refcount for slab cache destroy */
	void (*ctor)(void *);
	int inuse;		/* Offset to metadata */
	int align;		/* Alignment */
	int reserved;		/* Reserved bytes at the end of slabs */
	const char *name;	/* Name (only for display!) */
	struct list_head list;	/* List of slab caches */
#ifdef CONFIG_SYSFS // CONFIG_SYSFS=y
	// sizeof(struct kobject): 36 bytes
	struct kobject kobj;	/* For sysfs */
#endif
#ifdef CONFIG_MEMCG_KMEM // CONFIG_MEMCG_KMEM=n
	struct memcg_cache_params *memcg_params;
	int max_attr_size; /* for propagation, maximum size of a stored attr */
#endif

#ifdef CONFIG_NUMA // CONFIG_NUMA=n
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	int remote_node_defrag_ratio;
#endif
	// MAX_NUMNODES: 1
	struct kmem_cache_node *node[MAX_NUMNODES];
};

#endif /* _LINUX_SLUB_DEF_H */
