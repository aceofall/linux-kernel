/*
 * include/linux/idr.h
 * 
 * 2002-10-18  written by Jim Houston jim.houston@ccur.com
 *	Copyright (C) 2002 by Concurrent Computer Corporation
 *	Distributed under the GNU GPL license version 2.
 *
 * Small id to pointer translation service avoiding fixed sized
 * tables.
 */

#ifndef __IDR_H__
#define __IDR_H__

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/rcupdate.h>

/*
 * We want shallower trees and thus more bits covered at each layer.  8
 * bits gives us large enough first layer for most use cases and maximum
 * tree depth of 4.  Each idr_layer is slightly larger than 2k on 64bit and
 * 1k on 32bit.
 */
// ARM10C 20140920
// ARM10C 20151031
// ARM10C 20151107
// IDR_BITS: 8
#define IDR_BITS 8
// ARM10C 20140920
// ARM10C 20151107
// IDR_BITS: 8
// IDR_SIZE: 0x100
#define IDR_SIZE (1 << IDR_BITS)
// ARM10C 20151107
// IDR_BITS: 8
// IDR_MASK: 0xFF
#define IDR_MASK ((1 << IDR_BITS)-1)

// ARM10C 20140920
// ARM10C 20151031
// ARM10C 20151107
// ARM10C 20160213
// DECLARE_BITMAP(bitmap, 0x100): bitmap[8]
// sizeof(struct idr_layer): 1076 bytes
struct idr_layer {
	int			prefix;	/* the ID prefix of this idr_layer */
	// IDR_SIZE: 0x100
	DECLARE_BITMAP(bitmap, IDR_SIZE); /* A zero bit means "space here" */
	// IDR_BITS: 8
	struct idr_layer __rcu	*ary[1<<IDR_BITS];
	int			count;	/* When zero, we can release it */
	int			layer;	/* distance from leaf */
	struct rcu_head		rcu_head;
};

// ARM10C 20150808
// ARM10C 20151031
// sizeof(struct idr): 40 bytes
struct idr {
	struct idr_layer __rcu	*hint;	/* the last layer allocated from */
	struct idr_layer __rcu	*top;
	struct idr_layer	*id_free;
	int			layers;	/* only valid w/o concurrent changes */
	int			id_free_cnt;
	int			cur;	/* current pos for cyclic allocation */
	spinlock_t		lock;
};

// ARM10C 20151031
// __SPIN_LOCK_UNLOCKED((mnt_id_ida).idr.lock):
// (spinlock_t )
// { { .rlock =
//     {
//       .raw_lock = { { 0 } },
//       .magic = 0xdead4ead,
//       .owner_cpu = -1,
//       .owner = 0xffffffff,
//     }
// } }
//
// #define IDR_INIT((mnt_id_ida).idr):
// {
//      .lock =
//      (spinlock_t )
//      { { .rlock =
//          {
//            .raw_lock = { { 0 } },
//            .magic = 0xdead4ead,
//            .owner_cpu = -1,
//            .owner = 0xffffffff,
//          }
//      } },
// }
#define IDR_INIT(name)							\
{									\
	.lock			= __SPIN_LOCK_UNLOCKED(name.lock),	\
}
#define DEFINE_IDR(name)	struct idr name = IDR_INIT(name)

/**
 * DOC: idr sync
 * idr synchronization (stolen from radix-tree.h)
 *
 * idr_find() is able to be called locklessly, using RCU. The caller must
 * ensure calls to this function are made within rcu_read_lock() regions.
 * Other readers (lock-free or otherwise) and modifications may be running
 * concurrently.
 *
 * It is still required that the caller manage the synchronization and
 * lifetimes of the items. So if RCU lock-free lookups are used, typically
 * this would mean that the items have their own locks, or are amenable to
 * lock-free access; and that the items are freed by RCU (or only freed after
 * having been deleted from the idr tree *and* a synchronize_rcu() grace
 * period).
 */

/*
 * This is what we export.
 */

void *idr_find_slowpath(struct idr *idp, int id);
void idr_preload(gfp_t gfp_mask);
int idr_alloc(struct idr *idp, void *ptr, int start, int end, gfp_t gfp_mask);
int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp_mask);
int idr_for_each(struct idr *idp,
		 int (*fn)(int id, void *p, void *data), void *data);
void *idr_get_next(struct idr *idp, int *nextid);
void *idr_replace(struct idr *idp, void *ptr, int id);
void idr_remove(struct idr *idp, int id);
void idr_free(struct idr *idp, int id);
void idr_destroy(struct idr *idp);
void idr_init(struct idr *idp);

/**
 * idr_preload_end - end preload section started with idr_preload()
 *
 * Each idr_preload() should be matched with an invocation of this
 * function.  See idr_preload() for details.
 */
static inline void idr_preload_end(void)
{
	preempt_enable();
}

/**
 * idr_find - return pointer for given id
 * @idr: idr handle
 * @id: lookup key
 *
 * Return the pointer given the id it has been registered with.  A %NULL
 * return indicates that @id is not valid or you passed %NULL in
 * idr_get_new().
 *
 * This function can be called under rcu_read_lock(), given that the leaf
 * pointers lifetimes are correctly managed.
 */
static inline void *idr_find(struct idr *idr, int id)
{
	struct idr_layer *hint = rcu_dereference_raw(idr->hint);

	if (hint && (id & ~IDR_MASK) == hint->prefix)
		return rcu_dereference_raw(hint->ary[id & IDR_MASK]);

	return idr_find_slowpath(idr, id);
}

/**
 * idr_for_each_entry - iterate over an idr's elements of a given type
 * @idp:     idr handle
 * @entry:   the type * to use as cursor
 * @id:      id entry's key
 *
 * @entry and @id do not need to be initialized before the loop, and
 * after normal terminatinon @entry is left with the value NULL.  This
 * is convenient for a "not found" value.
 */
#define idr_for_each_entry(idp, entry, id)			\
	for (id = 0; ((entry) = idr_get_next(idp, &(id))) != NULL; ++id)

/*
 * Don't use the following functions.  These exist only to suppress
 * deprecated warnings on EXPORT_SYMBOL()s.
 */
int __idr_pre_get(struct idr *idp, gfp_t gfp_mask);
int __idr_get_new_above(struct idr *idp, void *ptr, int starting_id, int *id);
void __idr_remove_all(struct idr *idp);

/**
 * idr_pre_get - reserve resources for idr allocation
 * @idp:	idr handle
 * @gfp_mask:	memory allocation flags
 *
 * Part of old alloc interface.  This is going away.  Use
 * idr_preload[_end]() and idr_alloc() instead.
 */
static inline int __deprecated idr_pre_get(struct idr *idp, gfp_t gfp_mask)
{
	return __idr_pre_get(idp, gfp_mask);
}

/**
 * idr_get_new_above - allocate new idr entry above or equal to a start id
 * @idp: idr handle
 * @ptr: pointer you want associated with the id
 * @starting_id: id to start search at
 * @id: pointer to the allocated handle
 *
 * Part of old alloc interface.  This is going away.  Use
 * idr_preload[_end]() and idr_alloc() instead.
 */
static inline int __deprecated idr_get_new_above(struct idr *idp, void *ptr,
						 int starting_id, int *id)
{
	return __idr_get_new_above(idp, ptr, starting_id, id);
}

/**
 * idr_get_new - allocate new idr entry
 * @idp: idr handle
 * @ptr: pointer you want associated with the id
 * @id: pointer to the allocated handle
 *
 * Part of old alloc interface.  This is going away.  Use
 * idr_preload[_end]() and idr_alloc() instead.
 */
static inline int __deprecated idr_get_new(struct idr *idp, void *ptr, int *id)
{
	return __idr_get_new_above(idp, ptr, 0, id);
}

/**
 * idr_remove_all - remove all ids from the given idr tree
 * @idp: idr handle
 *
 * If you're trying to destroy @idp, calling idr_destroy() is enough.
 * This is going away.  Don't use.
 */
static inline void __deprecated idr_remove_all(struct idr *idp)
{
	__idr_remove_all(idp);
}

/*
 * IDA - IDR based id allocator, use when translation from id to
 * pointer isn't necessary.
 *
 * IDA_BITMAP_LONGS is calculated to be one less to accommodate
 * ida_bitmap->nr_busy so that the whole struct fits in 128 bytes.
 */
// ARM10C 20151031
// IDA_CHUNK_SIZE: 128
#define IDA_CHUNK_SIZE		128	/* 128 bytes per chunk */
// ARM10C 20151031
// IDA_CHUNK_SIZE: 128
// IDA_BITMAP_LONGS: 31
#define IDA_BITMAP_LONGS	(IDA_CHUNK_SIZE / sizeof(long) - 1)
// ARM10C 20151031
// ARM10C 20151107
// IDA_BITMAP_LONGS: 31
// IDA_BITMAP_BITS: 992
#define IDA_BITMAP_BITS 	(IDA_BITMAP_LONGS * sizeof(long) * 8)

// ARM10C 20151031
// ARM10C 20151107
// ARM10C 20160213
// ARM10C 20160305
// ARM10C 20160319
// sizeof(struct ida_bitmap): 128 bytes
struct ida_bitmap {
	long			nr_busy;
	// IDA_BITMAP_LONGS: 31
	unsigned long		bitmap[IDA_BITMAP_LONGS];
};

// ARM10C 20151031
// ARM10C 20160116
// ARM10C 20160213
struct ida {
	struct idr		idr;
	struct ida_bitmap	*free_bitmap;
};

// ARM10C 20151031
// IDR_INIT((mnt_id_ida).idr):
// {
//      .lock =
//      (spinlock_t )
//      { { .rlock =
//          {
//            .raw_lock = { { 0 } },
//            .magic = 0xdead4ead,
//            .owner_cpu = -1,
//            .owner = 0xffffffff,
//          }
//      } },
// }
//
// #define IDA_INIT(mnt_id_ida):
// {
//     .idr =
//     {
//         .lock =
//         (spinlock_t )
//         { { .rlock =
//              {
//                .raw_lock = { { 0 } },
//                .magic = 0xdead4ead,
//                .owner_cpu = -1,
//                .owner = 0xffffffff,
//              }
//          } },
//      }
//      .free_bitmap = NULL,
// }
#define IDA_INIT(name)		{ .idr = IDR_INIT((name).idr), .free_bitmap = NULL, }
// ARM10C 20151031
// IDA_INIT(mnt_id_ida):
// {
//     .idr =
//     {
//         .lock =
//         (spinlock_t )
//         { { .rlock =
//              {
//                .raw_lock = { { 0 } },
//                .magic = 0xdead4ead,
//                .owner_cpu = -1,
//                .owner = 0xffffffff,
//              }
//          } },
//      }
//      .free_bitmap = NULL,
// }
//
// #define DEFINE_IDA(mnt_id_ida):
// struct ida mnt_id_ida =
// {
//     .idr =
//     {
//         .lock =
//         (spinlock_t )
//         { { .rlock =
//              {
//                .raw_lock = { { 0 } },
//                .magic = 0xdead4ead,
//                .owner_cpu = -1,
//                .owner = 0xffffffff,
//              }
//          } },
//      }
//      .free_bitmap = NULL,
// }
// ARM10C 20151114
// #define DEFINE_IDA(unnamed_dev_ida):
// struct ida unnamed_dev_ida =
// {
//     .idr =
//     {
//         .lock =
//         (spinlock_t )
//         { { .rlock =
//              {
//                .raw_lock = { { 0 } },
//                .magic = 0xdead4ead,
//                .owner_cpu = -1,
//                .owner = 0xffffffff,
//              }
//          } },
//      }
//      .free_bitmap = NULL,
// }
// ARM10C 20160116
// #define DEFINE_IDA(sysfs_ino_ida):
// struct ida sysfs_ino_ida =
// {
//     .idr =
//     {
//         .lock =
//         (spinlock_t )
//         { { .rlock =
//              {
//                .raw_lock = { { 0 } },
//                .magic = 0xdead4ead,
//                .owner_cpu = -1,
//                .owner = 0xffffffff,
//              }
//          } },
//      }
//      .free_bitmap = NULL,
// }
//
// #define DEFINE_IDA(proc_inum_ida):
// struct ida proc_inum_ida =
// {
//     .idr =
//     {
//         .lock =
//         (spinlock_t )
//         { { .rlock =
//              {
//                .raw_lock = { { 0 } },
//                .magic = 0xdead4ead,
//                .owner_cpu = -1,
//                .owner = 0xffffffff,
//              }
//          } },
//      }
//      .free_bitmap = NULL,
// }
#define DEFINE_IDA(name)	struct ida name = IDA_INIT(name)

int ida_pre_get(struct ida *ida, gfp_t gfp_mask);
int ida_get_new_above(struct ida *ida, int starting_id, int *p_id);
void ida_remove(struct ida *ida, int id);
void ida_destroy(struct ida *ida);
void ida_init(struct ida *ida);

int ida_simple_get(struct ida *ida, unsigned int start, unsigned int end,
		   gfp_t gfp_mask);
void ida_simple_remove(struct ida *ida, unsigned int id);

/**
 * ida_get_new - allocate new ID
 * @ida:	idr handle
 * @p_id:	pointer to the allocated handle
 *
 * Simple wrapper around ida_get_new_above() w/ @starting_id of zero.
 */
// ARM10C 20160514
// &proc_inum_ida, &i
// ARM10C 20160604
// &proc_inum_ida, &i
static inline int ida_get_new(struct ida *ida, int *p_id)
{
	// ida: &proc_inum_ida, p_id: &i
	// ida: &proc_inum_ida, p_id: &i
	return ida_get_new_above(ida, 0, p_id);

	// ida_get_new_above에서 한일:
	// (&(&proc_inum_ida)->idr)->id_free: kmem_cache#21-oX (idr object 6)
	// (&(&proc_inum_ida)->idr)->id_free_cnt: 6
	// (&(&proc_inum_ida)->idr)->layers: 1
	// ((&(&proc_inum_ida)->idr)->top): kmem_cache#21-oX (idr object 8)
	//
	// (kmem_cache#21-oX (idr object 8))->layer: 0
	// kmem_cache#21-oX (struct idr_layer) (idr object 8)
	// ((kmem_cache#21-oX (struct idr_layer) (idr object 8))->ary[0]): (typeof(*kmem_cache#27-oX (struct ida_bitmap)) __force space *)(kmem_cache#27-oX (struct ida_bitmap))
	// (kmem_cache#21-oX (struct idr_layer) (idr object 8))->count: 1
	//
	// (&proc_inum_ida)->free_bitmap: NULL
	// kmem_cache#27-oX (struct ida_bitmap) 메모리을 0으로 초기화
	// (kmem_cache#27-oX (struct ida_bitmap))->nr_busy: 1
	// (kmem_cache#27-oX (struct ida_bitmap))->bitmap 의 0 bit를 1로 set 수행
	//
	// *p_id: 0
	//
	// kmem_cache인 kmem_cache#21 에서 할당한 object인 kmem_cache#21-oX (idr object 7) 의 memory 공간을 반환함

	// ida_get_new_above에서 한일:
	// (&(&proc_inum_ida)->idr)->top: kmem_cache#21-oX (struct idr_layer) (idr object 8)
	// (&(&proc_inum_ida)->idr)->layers: 1
	// (&(&proc_inum_ida)->idr)->id_free: (idr object new 0)
	// (&(&proc_inum_ida)->idr)->id_free_cnt: 7
	//
	// (kmem_cache#27-oX (struct ida_bitmap))->bitmap 의 1 bit를 1로 set 수행
	// (kmem_cache#27-oX (struct ida_bitmap))->nr_busy: 2
	//
	// *p_id: 1
	//
	// kmem_cache인 kmem_cache#21 에서 할당한 object인 kmem_cache#21-oX (idr object new 1) 의 memory 공간을 반환함
}

void __init idr_init_cache(void);

#endif /* __IDR_H__ */
