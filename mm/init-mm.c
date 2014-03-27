#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>

#include <linux/atomic.h>
#include <asm/pgtable.h>
#include <asm/mmu.h>

#ifndef INIT_MM_CONTEXT
// ARM10C 20131012
// KID 20140327
#define INIT_MM_CONTEXT(name)
#endif

// ARM10C 20131012
// KID 20140305
// KID 20140327
// RB_ROOT: (struct rb_root) { NULL, }
// swapper_pg_dir: 0xc0004000
// ATOMIC_INIT(1): { (1) }
// ATOMIC_INIT(2): { (2) }
// __RWSEM_INITIALIZER(init_mm.mmap_sem):
// {
//	0x00000000,
//	(raw_spinlock_t)
//	{
//		.raw_lock = { { 0 } },
//		.magic = 0xdead4ead,
//		.owner_cpu = -1,
//		.owner = 0xffffffff,
//	},
//	{ &((init_mm.mmap_sem).wait_list), &((init_mm.mmap_sem).wait_list) }
// }
// __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock):
//	(spinlock_t )
//	{ { .rlock =
//	    {
//	      .raw_lock = { { 0 } },
//	      .magic = 0xdead4ead,
//	      .owner_cpu = -1,
//	      .owner = 0xffffffff,
//	    }
//	} }
// LIST_HEAD_INIT(init_mm.mmlist):
// { &(init_mm.mmlist), &(init_mm.mmlist) }
// INIT_MM_CONTEXT(init_mm):
//
// struct mm_struct init_mm = {
// 	.mm_rb		= (struct rb_root) { NULL, },
// 	.pgd		= 0xc0004000,
// 	.mm_users	= { (2) },
// 	.mm_count	= { (1) },
// 	.mmap_sem	=
//	{
//		0x00000000,
//		(raw_spinlock_t)
//		{
//			.raw_lock = { { 0 } },
//			.magic = 0xdead4ead,
//			.owner_cpu = -1,
//			.owner = 0xffffffff,
//		},
//		{ &((init_mm.mmap_sem).wait_list), &((init_mm.mmap_sem).wait_list) }
//	}
//
// 	.page_table_lock =
//	(spinlock_t )
//	{ { .rlock =
//	    {
//	      .raw_lock = { { 0 } },
//	      .magic = 0xdead4ead,
//	      .owner_cpu = -1,
//	      .owner = 0xffffffff,
//	    }
//	} }
// 	.mmlist		= { &(init_mm.mmlist), &(init_mm.mmlist) },
// };
struct mm_struct init_mm = {
	.mm_rb		= RB_ROOT,
	// 연산결과 swapper_pg_dir : 0xc0004000
	.pgd		= swapper_pg_dir,
	.mm_users	= ATOMIC_INIT(2),
	.mm_count	= ATOMIC_INIT(1),
	.mmap_sem	= __RWSEM_INITIALIZER(init_mm.mmap_sem),
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock),
	.mmlist		= LIST_HEAD_INIT(init_mm.mmlist),
	INIT_MM_CONTEXT(init_mm)
};
