#ifndef __LINUX_MUTEX_DEBUG_H
#define __LINUX_MUTEX_DEBUG_H

#include <linux/linkage.h>
#include <linux/lockdep.h>
#include <linux/debug_locks.h>

/*
 * Mutexes - debugging helpers:
 */

// ARM10C 20140315
// __DEBUG_MUTEX_INITIALIZER(cpu_add_remove_lock):
// , .magic = &cpu_add_remove_lock
#define __DEBUG_MUTEX_INITIALIZER(lockname)				\
	, .magic = &lockname

// ARM10C 20150718
// &buf->lock: &(&(&(kmem_cache#25-oX)->port)->buf)->lock
// ARM10C 20150718
// &port->mutex: &(&(kmem_cache#25-oX)->port)->mutex
// ARM10C 20150718
// &port->buf_mutex: &(&(kmem_cache#25-oX)->port)->buf_mutex
// ARM10C 20150808
// &cgrp->pidlist_mutex: &(&(&cgroup_dummy_root)->top_cgroup)->pidlist_mutex
#define mutex_init(mutex)						\
do {									\
	static struct lock_class_key __key;				\
									\
	__mutex_init((mutex), #mutex, &__key);				\
} while (0)

extern void mutex_destroy(struct mutex *lock);

#endif
