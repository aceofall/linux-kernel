/*
 * Slab allocator functions that are independent of the allocator strategy
 *
 * (C) 2012 Christoph Lameter <cl@linux.com>
 */
#include <linux/slab.h>

#include <linux/mm.h>
#include <linux/poison.h>
#include <linux/interrupt.h>
#include <linux/memory.h>
#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/page.h>
#include <linux/memcontrol.h>
#include <trace/events/kmem.h>

#include "slab.h"

// ARM10C 20131207
// ARM10C 20140426
// ARM10C 20140607
enum slab_state slab_state;
// ARM10C 20140705
// ARM10C 20140712
LIST_HEAD(slab_caches);
DEFINE_MUTEX(slab_mutex);
// ARM10C 20140419
// ARM10C 20140628
struct kmem_cache *kmem_cache;

#ifdef CONFIG_DEBUG_VM
static int kmem_cache_sanity_check(struct mem_cgroup *memcg, const char *name,
				   size_t size)
{
	struct kmem_cache *s = NULL;

	if (!name || in_interrupt() || size < sizeof(void *) ||
		size > KMALLOC_MAX_SIZE) {
		pr_err("kmem_cache_create(%s) integrity check failed\n", name);
		return -EINVAL;
	}

	list_for_each_entry(s, &slab_caches, list) {
		char tmp;
		int res;

		/*
		 * This happens when the module gets unloaded and doesn't
		 * destroy its slab cache and no-one else reuses the vmalloc
		 * area of the module.  Print a warning.
		 */
		res = probe_kernel_address(s->name, tmp);
		if (res) {
			pr_err("Slab cache with size %d has lost its name\n",
			       s->object_size);
			continue;
		}

#if !defined(CONFIG_SLUB) || !defined(CONFIG_SLUB_DEBUG_ON)
		/*
		 * For simplicity, we won't check this in the list of memcg
		 * caches. We have control over memcg naming, and if there
		 * aren't duplicates in the global list, there won't be any
		 * duplicates in the memcg lists as well.
		 */
		if (!memcg && !strcmp(s->name, name)) {
			pr_err("%s (%s): Cache name already exists.\n",
			       __func__, name);
			dump_stack();
			s = NULL;
			return -EINVAL;
		}
#endif
	}

	WARN_ON(strchr(name, ' '));	/* It confuses parsers */
	return 0;
}
#else
static inline int kmem_cache_sanity_check(struct mem_cgroup *memcg,
					  const char *name, size_t size)
{
	return 0;
}
#endif

#ifdef CONFIG_MEMCG_KMEM
int memcg_update_all_caches(int num_memcgs)
{
	struct kmem_cache *s;
	int ret = 0;
	mutex_lock(&slab_mutex);

	list_for_each_entry(s, &slab_caches, list) {
		if (!is_root_cache(s))
			continue;

		ret = memcg_update_cache_size(s, num_memcgs);
		/*
		 * See comment in memcontrol.c, memcg_update_cache_size:
		 * Instead of freeing the memory, we'll just leave the caches
		 * up to this point in an updated state.
		 */
		if (ret)
			goto out;
	}

	memcg_update_array_size(num_memcgs);
out:
	mutex_unlock(&slab_mutex);
	return ret;
}
#endif

/*
 * Figure out what the alignment of the objects will be given a set of
 * flags, a user specified alignment and the size of the objects.
 */
// ARM10C 20140419
// flags: SLAB_HWCACHE_ALIGN: 0x00002000UL, ARCH_KMALLOC_MINALIGN: 64, size: 44
// ARM10C 20140614
// flags: SLAB_HWCACHE_ALIGN: 0x00002000UL, ARCH_KMALLOC_MINALIGN: 64, size: 116
unsigned long calculate_alignment(unsigned long flags,
		unsigned long align, unsigned long size)
{
	/*
	 * If the user wants hardware cache aligned objects then follow that
	 * suggestion if the object is sufficiently large.
	 *
	 * The hardware cache alignment cannot override the specified
	 * alignment though. If that is greater then use it.
	 */
	// flags: SLAB_HWCACHE_ALIGN
	if (flags & SLAB_HWCACHE_ALIGN) {
		// cache_line_size(): 64
		unsigned long ralign = cache_line_size();
		// ralign: 64

		// size : 44, ralign: 64
		// size : 116, ralign: 64
		while (size <= ralign / 2)
			ralign /= 2;

		// align: 64, ralign: 64
		align = max(align, ralign);
		// align: 64
	}

	// align: 64, ARCH_SLAB_MINALIGN: 8
	if (align < ARCH_SLAB_MINALIGN)
		align = ARCH_SLAB_MINALIGN;

	// align: 64, sizeof(void *): 4
	return ALIGN(align, sizeof(void *));
	// return 64
}


/*
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @align: The required alignment for the objects.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a interrupt, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache.
 *
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */

struct kmem_cache *
kmem_cache_create_memcg(struct mem_cgroup *memcg, const char *name, size_t size,
			size_t align, unsigned long flags, void (*ctor)(void *),
			struct kmem_cache *parent_cache)
{
	struct kmem_cache *s = NULL;
	int err = 0;

	get_online_cpus();
	mutex_lock(&slab_mutex);

	if (!kmem_cache_sanity_check(memcg, name, size) == 0)
		goto out_locked;

	/*
	 * Some allocators will constraint the set of valid flags to a subset
	 * of all flags. We expect them to define CACHE_CREATE_MASK in this
	 * case, and we'll just provide them with a sanitized version of the
	 * passed flags.
	 */
	flags &= CACHE_CREATE_MASK;

	s = __kmem_cache_alias(memcg, name, size, align, flags, ctor);
	if (s)
		goto out_locked;

	s = kmem_cache_zalloc(kmem_cache, GFP_KERNEL);
	if (s) {
		s->object_size = s->size = size;
		s->align = calculate_alignment(flags, align, size);
		s->ctor = ctor;

		if (memcg_register_cache(memcg, s, parent_cache)) {
			kmem_cache_free(kmem_cache, s);
			err = -ENOMEM;
			goto out_locked;
		}

		s->name = kstrdup(name, GFP_KERNEL);
		if (!s->name) {
			kmem_cache_free(kmem_cache, s);
			err = -ENOMEM;
			goto out_locked;
		}

		err = __kmem_cache_create(s, flags);
		if (!err) {
			s->refcount = 1;
			list_add(&s->list, &slab_caches);
			memcg_cache_list_add(memcg, s);
		} else {
			kfree(s->name);
			kmem_cache_free(kmem_cache, s);
		}
	} else
		err = -ENOMEM;

out_locked:
	mutex_unlock(&slab_mutex);
	put_online_cpus();

	if (err) {

		if (flags & SLAB_PANIC)
			panic("kmem_cache_create: Failed to create slab '%s'. Error %d\n",
				name, err);
		else {
			printk(KERN_WARNING "kmem_cache_create(%s) failed with error %d",
				name, err);
			dump_stack();
		}

		return NULL;
	}

	return s;
}

struct kmem_cache *
kmem_cache_create(const char *name, size_t size, size_t align,
		  unsigned long flags, void (*ctor)(void *))
{
	return kmem_cache_create_memcg(NULL, name, size, align, flags, ctor, NULL);
}
EXPORT_SYMBOL(kmem_cache_create);

void kmem_cache_destroy(struct kmem_cache *s)
{
	/* Destroy all the children caches if we aren't a memcg cache */
	kmem_cache_destroy_memcg_children(s);

	get_online_cpus();
	mutex_lock(&slab_mutex);
	s->refcount--;
	if (!s->refcount) {
		list_del(&s->list);

		if (!__kmem_cache_shutdown(s)) {
			mutex_unlock(&slab_mutex);
			if (s->flags & SLAB_DESTROY_BY_RCU)
				rcu_barrier();

			memcg_release_cache(s);
			kfree(s->name);
			kmem_cache_free(kmem_cache, s);
		} else {
			list_add(&s->list, &slab_caches);
			mutex_unlock(&slab_mutex);
			printk(KERN_ERR "kmem_cache_destroy %s: Slab cache still has objects\n",
				s->name);
			dump_stack();
		}
	} else {
		mutex_unlock(&slab_mutex);
	}
	put_online_cpus();
}
EXPORT_SYMBOL(kmem_cache_destroy);

// ARM10C 20131207
// ARM10C 20140607
int slab_is_available(void)
{
	return slab_state >= UP;
}

#ifndef CONFIG_SLOB // CONFIG_SLOB=n
/* Create a cache during boot when no slab services are available yet */
// ARM10C 20140419
// &boot_kmem_cache_node, "kmem_cache_node", sizeof(struct kmem_cache_node): 44 byte,
// SLAB_HWCACHE_ALIGN: 0x00002000UL
// ARM10C 20140614
// &boot_kmem_cache, "kmem_cache", 116, SLAB_HWCACHE_ALIGN: 0x00002000UL
void __init create_boot_cache(struct kmem_cache *s, const char *name, size_t size,
		unsigned long flags)
{
	int err;

	// s->name: boot_kmem_cache_node.name: NULL
	// s->name: boot_kmem_cache.name: NULL
	s->name = name;
	// s->name: boot_kmem_cache_node.name: "kmem_cache_node"
	// s->name: boot_kmem_cache.name: "kmem_cache"

	// s->size: boot_kmem_cache_node.size: 0
	// s->object_size: boot_kmem_cache_node.object_size: 0
	// s->size: boot_kmem_cache.size: 0
	// s->object_size: boot_kmem_cache.object_size: 0
	s->size = s->object_size = size;
	// s->size: boot_kmem_cache_node.size: 44
	// s->object_size: boot_kmem_cache_node.object_size: 44
	// s->size: boot_kmem_cache.size: 116
	// s->object_size: boot_kmem_cache.object_size: 116
	
	// flags: SLAB_HWCACHE_ALIGN: 0x00002000UL, ARCH_KMALLOC_MINALIGN: 64, size: 44
	// s->align: boot_kmem_cache_node.align: 0
	// flags: SLAB_HWCACHE_ALIGN: 0x00002000UL, ARCH_KMALLOC_MINALIGN: 64, size: 116
	// s->align: boot_kmem_cache.align: 0
	s->align = calculate_alignment(flags, ARCH_KMALLOC_MINALIGN, size);
	// s->align: boot_kmem_cache_node.align: 64
	// s->align: boot_kmem_cache.align: 64
	
	// s: &boot_kmem_cache_node, flags: SLAB_HWCACHE_ALIGN: 0x00002000UL
	// __kmem_cache_create(&boot_kmem_cache_node, 0x00002000UL): 0
	// s: &boot_kmem_cache, flags: SLAB_HWCACHE_ALIGN: 0x00002000UL
	// __kmem_cache_create(&boot_kmem_cache, 0x00002000UL): 0
	err = __kmem_cache_create(s, flags);
	// err: 0
	// err: 0

	// __kmem_cache_create(&boot_kmem_cache_node) 가 한일:
	// boot_kmem_cache_node.flags: SLAB_HWCACHE_ALIGN: 0x00002000UL
	// boot_kmem_cache_node.reserved: 0
	// boot_kmem_cache_node.min_partial: 5
	// boot_kmem_cache_node.cpu_partial: 30
	//
	// migratetype이 MIGRATE_UNMOVABLE인 page 할당 받음
	// page 맴버를 셋팅함
	// page->slab_cache: &boot_kmem_cache_node 주소를 set
	// page->flags에 7 (PG_slab) bit를 set
	// page->freelist: UNMOVABLE인 page 의 object의 시작 virtual address + 64
	// page->inuse: 1, page->frozen: 0 page 맴버를 셋팅함
	// slab 의 objects 들의 freepointer를 맵핑함
	// 할당받은 slab object를 kmem_cache_node 로 사용하고 kmem_cache_node의 멤버 필드를 초기화함
	// kmem_cache_node->nr_partial: 1
	// kmem_cache_node->list_lock: spinlock 초기화 수행
	// kmem_cache_node->slabs: 1, kmem_cache_node->total_objects: 64 로 세팀함
	// kmem_cache_node->full: 리스트 초기화
	// kmem_cache_node의 partial 맴버에 현재 page의 lru 리스트를 추가함
	//
	// 할당받은 pcpu 들의 16 byte 공간 (&boot_kmem_cache_node)->cpu_slab 에
	// 각 cpu에 사용하는 kmem_cache_cpu의 tid 맵버를 설정

	// __kmem_cache_create(&boot_kmem_cache) 가 한일:
	// boot_kmem_cache.flags: SLAB_HWCACHE_ALIGN: 0x00002000UL
	// boot_kmem_cache.reserved: 0
	// boot_kmem_cache.min_partial: 5
	// boot_kmem_cache.cpu_partial: 30
	//
	// 할당 받아 놓은 migratetype이 MIGRATE_UNMOVABLE인 page 를 사용
	// page 맴버를 셋팅함
	// page->counters: 0x80200020
	// page->inuse: 32
	// page->objects: 32
	// page->frozen: 1
	// page->freelist: NULL
	// 할당받은 slab object를 kmem_cache_node 로 사용하고 kmem_cache_node의 멤버 필드를 초기화함
	// 첫번째 object:
	// kmem_cache_node->partial에 연결된 (MIGRATE_UNMOVABLE인 page)->lru 를 삭제
	// kmem_cache_node->nr_partial: 0
	// 두번째 object:
	// kmem_cache_node->nr_partial: 0
	// kmem_cache_node->list_lock: spinlock 초기화 수행
	// kmem_cache_node->slabs: 0, kmem_cache_node->total_objects: 0 로 세팀함
	// kmem_cache_node->full: 리스트 초기화
	//
	// kmem_cache_node 가 boot_kmem_cache.node[0]에 할당됨
	//
	// 할당받은 pcpu 들의 16 byte 공간 (&boot_kmem_cache)->cpu_slab 에
	// 각 cpu에 사용하는 kmem_cache_cpu의 tid 맵버를 설정

	// err: 0
	// err: 0
	if (err)
		panic("Creation of kmalloc slab %s size=%zu failed. Reason %d\n",
					name, size, err);

	// s->refcount: boot_kmem_cache_node.refcount
	// s->refcount: boot_kmem_cache.refcount
	s->refcount = -1;	/* Exempt from merging for now */
	// s->refcount: boot_kmem_cache_node.refcount: -1
	// s->refcount: boot_kmem_cache.refcount: -1
}

// ARM10C 20140719
// NULL, 64, 0
struct kmem_cache *__init create_kmalloc_cache(const char *name, size_t size,
				unsigned long flags)
{
	// kmem_cache: UNMOVABLE인 page (boot_kmem_cache)의 object의 시작 virtual address,
	// GFP_NOWAIT: 0
	// kmem_cache_zalloc(UNMOVABLE인 page (boot_kmem_cache)의 object의 시작 virtual address, 0):
	// UNMOVABLE인 page (boot_kmem_cache)의 시작 virtual address + 128
	struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
	// s: UNMOVABLE인 page (boot_kmem_cache)의 시작 virtual address + 128

	// UNMOVABLE인 page (boot_kmem_cache)의 시작 virtual address + 128 를
	// kmem_cache 용 2번째 object 인데 주석 추가의 용의성을 위해
	// kmem_cache#2 부르기로 함

// 2014/07/19 종료

	if (!s)
		panic("Out of memory when creating slab %s\n", name);

	create_boot_cache(s, name, size, flags);
	list_add(&s->list, &slab_caches);
	s->refcount = 1;
	return s;
}

// ARM10C 20140719
// KMALLOC_SHIFT_HIGH: 13
struct kmem_cache *kmalloc_caches[KMALLOC_SHIFT_HIGH + 1];
EXPORT_SYMBOL(kmalloc_caches);

#ifdef CONFIG_ZONE_DMA
struct kmem_cache *kmalloc_dma_caches[KMALLOC_SHIFT_HIGH + 1];
EXPORT_SYMBOL(kmalloc_dma_caches);
#endif

/*
 * Conversion table for small slabs sizes / 8 to the index in the
 * kmalloc array. This is necessary for slabs < 192 since we have non power
 * of two cache sizes there. The size of larger slabs can be determined using
 * fls.
 */
// ARM10C 20140719
static s8 size_index[24] = {
	3,	/* 8 */
	4,	/* 16 */
	5,	/* 24 */
	5,	/* 32 */
	6,	/* 40 */
	6,	/* 48 */
	6,	/* 56 */
	6,	/* 64 */
	1,	/* 72 */
	1,	/* 80 */
	1,	/* 88 */
	1,	/* 96 */
	7,	/* 104 */
	7,	/* 112 */
	7,	/* 120 */
	7,	/* 128 */
	2,	/* 136 */
	2,	/* 144 */
	2,	/* 152 */
	2,	/* 160 */
	2,	/* 168 */
	2,	/* 176 */
	2,	/* 184 */
	2	/* 192 */
};

// ARM10C 20140719
// i: 8
static inline int size_index_elem(size_t bytes)
{
	// bytes: 8
	return (bytes - 1) / 8;
	// return 0
}

/*
 * Find the kmem_cache structure that serves a given size of
 * allocation
 */
struct kmem_cache *kmalloc_slab(size_t size, gfp_t flags)
{
	int index;

	if (unlikely(size > KMALLOC_MAX_SIZE)) {
		WARN_ON_ONCE(!(flags & __GFP_NOWARN));
		return NULL;
	}

	if (size <= 192) {
		if (!size)
			return ZERO_SIZE_PTR;

		index = size_index[size_index_elem(size)];
	} else
		index = fls(size - 1);

#ifdef CONFIG_ZONE_DMA
	if (unlikely((flags & GFP_DMA)))
		return kmalloc_dma_caches[index];

#endif
	return kmalloc_caches[index];
}

/*
 * Create the kmalloc array. Some of the regular kmalloc arrays
 * may already have been created because they were needed to
 * enable allocations for slab creation.
 */
// ARM10C 20140719
// flags: 0
void __init create_kmalloc_caches(unsigned long flags)
{
	int i;

	/*
	 * Patch up the size_index table if we have strange large alignment
	 * requirements for the kmalloc array. This is only the case for
	 * MIPS it seems. The standard arches will not generate any code here.
	 *
	 * Largest permitted alignment is 256 bytes due to the way we
	 * handle the index determination for the smaller caches.
	 *
	 * Make sure that nothing crazy happens if someone starts tinkering
	 * around with ARCH_KMALLOC_MINALIGN
	 */
	// KMALLOC_MIN_SIZE: 64
	BUILD_BUG_ON(KMALLOC_MIN_SIZE > 256 ||
		(KMALLOC_MIN_SIZE & (KMALLOC_MIN_SIZE - 1)));

	// KMALLOC_MIN_SIZE: 64
	for (i = 8; i < KMALLOC_MIN_SIZE; i += 8) {
		// i: 8, size_index_elem(8): 0
		int elem = size_index_elem(i);
		// elem: 0

		// elem: 0, ARRAY_SIZE(size_index): 24
		if (elem >= ARRAY_SIZE(size_index))
			break;

		// elem: 0, KMALLOC_SHIFT_LOW: 6
		size_index[elem] = KMALLOC_SHIFT_LOW;
		// size_index[0]: 6
	}
	// 루프 수행 결과
	// size_index[0 .. 6]: 6

	// KMALLOC_MIN_SIZE: 64
	if (KMALLOC_MIN_SIZE >= 64) {
		/*
		 * The 96 byte size cache is not used if the alignment
		 * is 64 byte.
		 */
		for (i = 64 + 8; i <= 96; i += 8)
			// i: 72, size_index_elem(72): 8
			size_index[size_index_elem(i)] = 7;
			// size_index[8]: 7

		// 루프 수행 결과
		// size_index[8 .. 11]: 7
	}

	// KMALLOC_MIN_SIZE: 64
	if (KMALLOC_MIN_SIZE >= 128) {
		/*
		 * The 192 byte sized cache is not used if the alignment
		 * is 128 byte. Redirect kmalloc to use the 256 byte cache
		 * instead.
		 */
		for (i = 128 + 8; i <= 192; i += 8)
			size_index[size_index_elem(i)] = 8;
	}

	// KMALLOC_SHIFT_LOW: 6, KMALLOC_SHIFT_HIGH: 13
	for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++) {
		// i: 6, kmalloc_caches[6]: NULL
		if (!kmalloc_caches[i]) {

			// i: 6, flags: 0, create_kmalloc_cache(NULL, 64, 0)
			kmalloc_caches[i] = create_kmalloc_cache(NULL,
							1 << i, flags);
		}

		/*
		 * Caches that are not of the two-to-the-power-of size.
		 * These have to be created immediately after the
		 * earlier power of two caches
		 */
		if (KMALLOC_MIN_SIZE <= 32 && !kmalloc_caches[1] && i == 6)
			kmalloc_caches[1] = create_kmalloc_cache(NULL, 96, flags);

		if (KMALLOC_MIN_SIZE <= 64 && !kmalloc_caches[2] && i == 7)
			kmalloc_caches[2] = create_kmalloc_cache(NULL, 192, flags);
	}

	/* Kmalloc array is now usable */
	slab_state = UP;

	for (i = 0; i <= KMALLOC_SHIFT_HIGH; i++) {
		struct kmem_cache *s = kmalloc_caches[i];
		char *n;

		if (s) {
			n = kasprintf(GFP_NOWAIT, "kmalloc-%d", kmalloc_size(i));

			BUG_ON(!n);
			s->name = n;
		}
	}

#ifdef CONFIG_ZONE_DMA
	for (i = 0; i <= KMALLOC_SHIFT_HIGH; i++) {
		struct kmem_cache *s = kmalloc_caches[i];

		if (s) {
			int size = kmalloc_size(i);
			char *n = kasprintf(GFP_NOWAIT,
				 "dma-kmalloc-%d", size);

			BUG_ON(!n);
			kmalloc_dma_caches[i] = create_kmalloc_cache(n,
				size, SLAB_CACHE_DMA | flags);
		}
	}
#endif
}
#endif /* !CONFIG_SLOB */

#ifdef CONFIG_TRACING
void *kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order)
{
	void *ret = kmalloc_order(size, flags, order);
	trace_kmalloc(_RET_IP_, ret, size, PAGE_SIZE << order, flags);
	return ret;
}
EXPORT_SYMBOL(kmalloc_order_trace);
#endif

#ifdef CONFIG_SLABINFO

#ifdef CONFIG_SLAB
#define SLABINFO_RIGHTS (S_IWUSR | S_IRUSR)
#else
#define SLABINFO_RIGHTS S_IRUSR
#endif

void print_slabinfo_header(struct seq_file *m)
{
	/*
	 * Output format version, so at least we can change it
	 * without _too_ many complaints.
	 */
#ifdef CONFIG_DEBUG_SLAB
	seq_puts(m, "slabinfo - version: 2.1 (statistics)\n");
#else
	seq_puts(m, "slabinfo - version: 2.1\n");
#endif
	seq_puts(m, "# name            <active_objs> <num_objs> <objsize> "
		 "<objperslab> <pagesperslab>");
	seq_puts(m, " : tunables <limit> <batchcount> <sharedfactor>");
	seq_puts(m, " : slabdata <active_slabs> <num_slabs> <sharedavail>");
#ifdef CONFIG_DEBUG_SLAB
	seq_puts(m, " : globalstat <listallocs> <maxobjs> <grown> <reaped> "
		 "<error> <maxfreeable> <nodeallocs> <remotefrees> <alienoverflow>");
	seq_puts(m, " : cpustat <allochit> <allocmiss> <freehit> <freemiss>");
#endif
	seq_putc(m, '\n');
}

static void *s_start(struct seq_file *m, loff_t *pos)
{
	loff_t n = *pos;

	mutex_lock(&slab_mutex);
	if (!n)
		print_slabinfo_header(m);

	return seq_list_start(&slab_caches, *pos);
}

void *slab_next(struct seq_file *m, void *p, loff_t *pos)
{
	return seq_list_next(p, &slab_caches, pos);
}

void slab_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&slab_mutex);
}

static void
memcg_accumulate_slabinfo(struct kmem_cache *s, struct slabinfo *info)
{
	struct kmem_cache *c;
	struct slabinfo sinfo;
	int i;

	if (!is_root_cache(s))
		return;

	for_each_memcg_cache_index(i) {
		c = cache_from_memcg_idx(s, i);
		if (!c)
			continue;

		memset(&sinfo, 0, sizeof(sinfo));
		get_slabinfo(c, &sinfo);

		info->active_slabs += sinfo.active_slabs;
		info->num_slabs += sinfo.num_slabs;
		info->shared_avail += sinfo.shared_avail;
		info->active_objs += sinfo.active_objs;
		info->num_objs += sinfo.num_objs;
	}
}

int cache_show(struct kmem_cache *s, struct seq_file *m)
{
	struct slabinfo sinfo;

	memset(&sinfo, 0, sizeof(sinfo));
	get_slabinfo(s, &sinfo);

	memcg_accumulate_slabinfo(s, &sinfo);

	seq_printf(m, "%-17s %6lu %6lu %6u %4u %4d",
		   cache_name(s), sinfo.active_objs, sinfo.num_objs, s->size,
		   sinfo.objects_per_slab, (1 << sinfo.cache_order));

	seq_printf(m, " : tunables %4u %4u %4u",
		   sinfo.limit, sinfo.batchcount, sinfo.shared);
	seq_printf(m, " : slabdata %6lu %6lu %6lu",
		   sinfo.active_slabs, sinfo.num_slabs, sinfo.shared_avail);
	slabinfo_show_stats(m, s);
	seq_putc(m, '\n');
	return 0;
}

static int s_show(struct seq_file *m, void *p)
{
	struct kmem_cache *s = list_entry(p, struct kmem_cache, list);

	if (!is_root_cache(s))
		return 0;
	return cache_show(s, m);
}

/*
 * slabinfo_op - iterator that generates /proc/slabinfo
 *
 * Output layout:
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */
static const struct seq_operations slabinfo_op = {
	.start = s_start,
	.next = slab_next,
	.stop = slab_stop,
	.show = s_show,
};

static int slabinfo_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &slabinfo_op);
}

static const struct file_operations proc_slabinfo_operations = {
	.open		= slabinfo_open,
	.read		= seq_read,
	.write          = slabinfo_write,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init slab_proc_init(void)
{
	proc_create("slabinfo", SLABINFO_RIGHTS, NULL,
						&proc_slabinfo_operations);
	return 0;
}
module_init(slab_proc_init);
#endif /* CONFIG_SLABINFO */
