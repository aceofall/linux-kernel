/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 *  SMP-safe vmalloc/vfree/ioremap, Tigran Aivazian <tigran@veritas.com>, May 2000
 *  Major rework to support vmap/vunmap, Christoph Hellwig, SGI, August 2002
 *  Numa awareness, Christoph Lameter, SGI, June 2005
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/pfn.h>
#include <linux/kmemleak.h>
#include <linux/atomic.h>
#include <linux/llist.h>
#include <asm/uaccess.h>
#include <asm/tlbflush.h>
#include <asm/shmparam.h>

// ARM10C 20140726
// ARM10C 20140809
struct vfree_deferred {
	struct llist_head list;
	struct work_struct wq;
};

// ARM10C 20140809
static DEFINE_PER_CPU(struct vfree_deferred, vfree_deferred);

static void __vunmap(const void *, int);

// ARM10C 20140809
static void free_work(struct work_struct *w)
{
	struct vfree_deferred *p = container_of(w, struct vfree_deferred, wq);
	struct llist_node *llnode = llist_del_all(&p->list);
	while (llnode) {
		void *p = llnode;
		llnode = llist_next(llnode);
		__vunmap(p, 1);
	}
}

/*** Page table manipulation functions ***/

// ARM10C 20160827
// pmd: 가상주소가 포함되어 있는 pgd 값
// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
static void vunmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end)
{
	pte_t *pte;

	// pmd: 가상주소가 포함되어 있는 pgd 값
	// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
	pte = pte_offset_kernel(pmd, addr);
	// pte: 가상주소에 매핑 되어 있는 pte 값

	do {
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// pte: 가상주소에 매핑 되어 있는 pte 값
		// ptep_get_and_clear(&init_mm, (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// 가상주소에 매핑 되어 있는 pte 값): *(가상주소에 매핑 되어 있는 pte 값)
		pte_t ptent = ptep_get_and_clear(&init_mm, addr, pte);
		// ptent: *(가상주소에 매핑 되어 있는 pte 값)

		// ptep_get_and_clear 에서 한일:
		// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함

		// ptent: *(가상주소에 매핑 되어 있는 pte 값)
		// pte_none(*(가상주소에 매핑 되어 있는 pte 값)): 1
		// pte_present(*(가상주소에 매핑 되어 있는 pte 값)): 1
		WARN_ON(!pte_none(ptent) && !pte_present(ptent));

		// pte: 가상주소에 매핑 되어 있는 pte 값, PAGE_SIZE: 0x1000
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

// ARM10C 20160827
// pud: 가상주소가 포함되어 있는 pgd 값
// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
static void vunmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;

	// pud: 가상주소가 포함되어 있는 pgd 값
	// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
	pmd = pmd_offset(pud, addr);
	// pmd: 가상주소가 포함되어 있는 pgd 값

	do {
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		next = pmd_addr_end(addr, end);
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end

		// pmd: 가상주소가 포함되어 있는 pgd 값
		// pmd_none_or_clear_bad(가상주소가 포함되어 있는 pgd 값): 0
		if (pmd_none_or_clear_bad(pmd))
			continue;

		// pmd: 가상주소가 포함되어 있는 pgd 값
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		vunmap_pte_range(pmd, addr, next);

		// vunmap_pte_range 에서 한일:
		// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함

		// pmd: 가상주소가 포함되어 있는 pgd 값
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	} while (pmd++, addr = next, addr != end);
}

// ARM10C 20160827
// pgd: 가상주소가 포함되어 있는 pgd 값,
// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
static void vunmap_pud_range(pgd_t *pgd, unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next;

	// pgd: 가상주소가 포함되어 있는 pgd 값,
	// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start
	// pud_offset(가상주소가 포함되어 있는 pgd 값, (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start):
	// 가상주소가 포함되어 있는 pgd 값
	pud = pud_offset(pgd, addr);
	// pud: 가상주소가 포함되어 있는 pgd 값

	do {
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		next = pud_addr_end(addr, end);
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end

		// pud: 가상주소가 포함되어 있는 pgd 값
		// pud_none_or_clear_bad(가상주소가 포함되어 있는 pgd 값): 0
		if (pud_none_or_clear_bad(pud))
			continue;

		// pud: 가상주소가 포함되어 있는 pgd 값
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		vunmap_pmd_range(pud, addr, next);

		// vunmap_pmd_range 에서 한일:
		// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함

		// pud: 가상주소가 포함되어 있는 pgd 값
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	} while (pud++, addr = next, addr != end);
}

// ARM10C 20160827
// va->va_start: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
// va->va_end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
static void vunmap_page_range(unsigned long addr, unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
	// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	BUG_ON(addr >= end);

	// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start
	// pgd_offset_k((할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start):
	// 가상주소가 포함되어 있는 pgd 값
	pgd = pgd_offset_k(addr);

	// pgd: 가상주소가 포함되어 있는 pgd 값
	do {
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		next = pgd_addr_end(addr, end);
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end

		// pgd: 가상주소가 포함되어 있는 pgd 값
		// pgd_none_or_clear_bad(가상주소가 포함되어 있는 pgd 값): 0
		if (pgd_none_or_clear_bad(pgd))
			continue;

		// pgd: 가상주소가 포함되어 있는 pgd 값,
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		vunmap_pud_range(pgd, addr, next);

		// vunmap_pud_range 에서 한일:
		// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함

		// pgd: 가상주소가 포함되어 있는 pgd 값
		// addr: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// next: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end,
		// end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	} while (pgd++, addr = next, addr != end);
}

// ARM10C 20160820
// pmd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb),
// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000,
// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소), nr: &nr
static int vmap_pte_range(pmd_t *pmd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pte_t *pte;

	/*
	 * nr is a running index into the array which helps higher level
	 * callers keep track of where we're up to.
	 */

	// pmd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb), addr: 할당 받은 가상 주소값
	// pte_alloc_kernel(할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb), 할당 받은 가상 주소값)
	// 할당 받은 가상 주소값의 pte 값
	pte = pte_alloc_kernel(pmd, addr);
	// pte: 할당 받은 가상 주소값의 pte 값

	// pte: 할당 받은 가상 주소값의 pte 값
	if (!pte)
		return -ENOMEM;
	do {
		// pages: &(page 1개(4K)의 할당된 메모리 주소), *nr: nr: 0
		struct page *page = pages[*nr];
		// page: page 1개(4K)의 할당된 메모리 주소

		// pte: 할당 받은 가상 주소값의 pte 값, pte_none(*(할당 받은 가상 주소값의 pte 값)): 1
		if (WARN_ON(!pte_none(*pte)))
			return -EBUSY;

		// page: page 1개(4K)의 할당된 메모리 주소
		if (WARN_ON(!page))
			return -ENOMEM;

		// addr: 할당 받은 가상 주소값, pte: 할당 받은 가상 주소값의 pte 값,
		// page: page 1개(4K)의 할당된 메모리 주소, prot: pgprot_kernel에 0x204 를 or 한 값
		// mk_pte(page 1개(4K)의 할당된 메모리 주소, pgprot_kernel에 0x204 를 or 한 값): page 1개(4K)의 hw pte 값
		set_pte_at(&init_mm, addr, pte, mk_pte(page, prot));

		// set_pte_at에서 한일:
		// 할당 받은 가상 주소값의 pte 값을 page 1개(4K)의 hw pte 값을 갱신
		// (linux pgtable과 hardware pgtable의 값 같이 갱신)

		// *nr: nr: 0
		(*nr)++;
		// *nr: nr: 1

		// pte: 할당 받은 가상 주소값의 pte 값, PAGE_SIZE: 0x1000
		// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000
	} while (pte++, addr += PAGE_SIZE, addr != end);

	// 위 loop 에서 한일:
	// 할당 받은 가상 주소값을 가지고 있는 page table section 하위 pte table을 갱신함

	return 0;
	// return 0
}

// ARM10C 20160820
// pud: 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000,
// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소), nr: &nr
static int vmap_pmd_range(pud_t *pud, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pmd_t *pmd;
	unsigned long next;

	// pud: 할당 받은 가상 주소값에 맞는 MMU table의 section 값, addr: 할당 받은 가상 주소값
	// pmd_alloc(&init_mm, 할당 받은 가상 주소값에 맞는 MMU table의 section 값, 할당 받은 가상 주소값):
	// 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb)
	pmd = pmd_alloc(&init_mm, pud, addr);
	// pmd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb)

	// pmd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb)
	if (!pmd)
		return -ENOMEM;
	do {
		// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000
		// pmd_addr_end(할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000):
		// 할당 받은 가상 주소값에 맞는 MMU table의 다음 section 값
		next = pmd_addr_end(addr, end);
		// next: 할당 받은 가상 주소값+ 0x1000

		// pmd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb),
		// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000,
		// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소), nr: &nr
		// vmap_pte_range(할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb),
		// 할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000,
		// pgprot_kernel에 0x204 를 or 한 값, &(page 1개(4K)의 할당된 메모리 주소), &nr): 0
		if (vmap_pte_range(pmd, addr, next, prot, pages, nr))
			return -ENOMEM;

		// vmap_pte_range 에서 한일:
		// 할당 받은 가상 주소값을 가지고 있는 page table section 2개의 하위 pte table을 갱신함

		// pmd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값(pmd 1mb),
		// next: 할당 받은 가상 주소값에 맞는 MMU table의 다음 section 값,
		// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값에 맞는 MMU table의 다음 section 값
	} while (pmd++, addr = next, addr != end);

	return 0;
	// return 0
}

// ARM10C 20160820
// pgd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000,
// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소)
static int vmap_pud_range(pgd_t *pgd, unsigned long addr,
		unsigned long end, pgprot_t prot, struct page **pages, int *nr)
{
	pud_t *pud;
	unsigned long next;

	// pgd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값, addr: 할당 받은 가상 주소값
	// pud_alloc(&init_mm, 할당 받은 가상 주소값에 맞는 MMU table의 section 값, addr: 할당 받은 가상 주소값):
	// pgd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값
	pud = pud_alloc(&init_mm, pgd, addr);
	// pud: 할당 받은 가상 주소값에 맞는 MMU table의 section 값

	// pud: 할당 받은 가상 주소값에 맞는 MMU table의 section 값
	if (!pud)
		return -ENOMEM;
	do {
		// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000
		// pud_addr_end(할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000):
		// 할당 받은 가상 주소값에 맞는 MMU table의 다음 section 값
		next = pud_addr_end(addr, end);
		// next: 할당 받은 가상 주소값+ 0x1000

		// pud: 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
		// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000,
		// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소), nr: &nr
		// vmap_pmd_range( 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
		// 할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000,
		// pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소), &nr): 0
		if (vmap_pmd_range(pud, addr, next, prot, pages, nr))
			return -ENOMEM;

		// vmap_pmd_range 에서 한일:
		// 할당 받은 가상 주소값을 가지고 있는 page table section 하위 pte table을 갱신함

		// pud: 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
		// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000, end: 할당 받은 가상 주소값+ 0x1000
	} while (pud++, addr = next, addr != end);

	return 0;
	// return 0
}

/*
 * Set up page tables in kva (addr, end). The ptes shall have prot "prot", and
 * will have pfns corresponding to the "pages" array.
 *
 * Ie. pte at addr+N*PAGE_SIZE shall point to pfn corresponding to pages[N]
 */
// ARM10C 20160813
// start: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000,
// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소)
static int vmap_page_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages)
{
	pgd_t *pgd;
	unsigned long next;

	// start: 할당 받은 가상 주소값
	unsigned long addr = start;
	// addr: 할당 받은 가상 주소값

	int err = 0;
	// err: 0

	int nr = 0;
	// nr: 0

	// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000
	BUG_ON(addr >= end);

	// addr: 할당 받은 가상 주소값, pgd_offset_k(할당 받은 가상 주소값): 할당 받은 가상 주소값에 맞는 MMU table의 section 값
	pgd = pgd_offset_k(addr);
	// pgd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값

	do {
		// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000
		// pgd_addr_end(할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000): 할당 받은 가상 주소값에 맞는 MMU table의 다음 section 값
		next = pgd_addr_end(addr, end);
		// next: 할당 받은 가상 주소값+ 0x1000

		// pgd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
		// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000,
		// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소)
		// vmap_pud_range(할당 받은 가상 주소값에 맞는 MMU table의 section 값,
		// 할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000,
		// pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소), &nr): 0
		err = vmap_pud_range(pgd, addr, next, prot, pages, &nr);
		// err: 0

		// vmap_pud_range 에서 한일:
		// 할당 받은 가상 주소값을 가지고 있는 page table section 하위 pte table을 갱신함
		// nr: 1

		// err: 0
		if (err)
			return err;

		// pgd: 할당 받은 가상 주소값에 맞는 MMU table의 section 값,
		// addr: 할당 받은 가상 주소값, next: 할당 받은 가상 주소값+ 0x1000, end: 할당 받은 가상 주소값+ 0x1000,
	} while (pgd++, addr = next, addr != end);

	// nr: 1
	return nr;
	// return 1
}

// ARM10C 20160813
// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000,
// prot: pgprot_kernel에 0x204 를 or 한 값, *pages: &(page 1개(4K)의 할당된 메모리 주소)
static int vmap_page_range(unsigned long start, unsigned long end,
			   pgprot_t prot, struct page **pages)
{
	int ret;

// 2016/08/13 종료
// 2016/08/20 시작

	// start: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000,
	// prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소)
	// vmap_page_range_noflush( 할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000,
	// pgprot_kernel에 0x204 를 or 한 값, &(page 1개(4K)의 할당된 메모리 주소)): 1
	ret = vmap_page_range_noflush(start, end, prot, pages);
	// ret: 1

	// vmap_page_range_noflush 에서 한일:
	// 할당 받은 가상 주소값을 가지고 있는 page table section 하위 pte table을 갱신함

	// start: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000
	flush_cache_vmap(start, end);

	// flush_cache_vmap 에서 한일:
	// cache의 값을 전부 메모리에 반영

	// ret: 1
	return ret;
	// return 1
}

int is_vmalloc_or_module_addr(const void *x)
{
	/*
	 * ARM, x86-64 and sparc64 put modules in a special place,
	 * and fall back on vmalloc() if that fails. Others
	 * just put it in the vmalloc space.
	 */
#if defined(CONFIG_MODULES) && defined(MODULES_VADDR)
	unsigned long addr = (unsigned long)x;
	if (addr >= MODULES_VADDR && addr < MODULES_END)
		return 1;
#endif
	return is_vmalloc_addr(x);
}

/*
 * Walk a vmap address to the struct page it maps.
 */
struct page *vmalloc_to_page(const void *vmalloc_addr)
{
	unsigned long addr = (unsigned long) vmalloc_addr;
	struct page *page = NULL;
	pgd_t *pgd = pgd_offset_k(addr);

	/*
	 * XXX we might need to change this if we add VIRTUAL_BUG_ON for
	 * architectures that do not vmalloc module space
	 */
	VIRTUAL_BUG_ON(!is_vmalloc_or_module_addr(vmalloc_addr));

	if (!pgd_none(*pgd)) {
		pud_t *pud = pud_offset(pgd, addr);
		if (!pud_none(*pud)) {
			pmd_t *pmd = pmd_offset(pud, addr);
			if (!pmd_none(*pmd)) {
				pte_t *ptep, pte;

				ptep = pte_offset_map(pmd, addr);
				pte = *ptep;
				if (pte_present(pte))
					page = pte_page(pte);
				pte_unmap(ptep);
			}
		}
	}
	return page;
}
EXPORT_SYMBOL(vmalloc_to_page);

/*
 * Map a vmalloc()-space virtual address to the physical page frame number.
 */
unsigned long vmalloc_to_pfn(const void *vmalloc_addr)
{
	return page_to_pfn(vmalloc_to_page(vmalloc_addr));
}
EXPORT_SYMBOL(vmalloc_to_pfn);


/*** Global kva allocator ***/

// ARM10C 20160827
// VM_LAZY_FREE: 0x01
#define VM_LAZY_FREE	0x01
#define VM_LAZY_FREEING	0x02
// ARM10C 20140809
// ARM10C 20141025
// ARM10C 20160827
// VM_VM_AREA: 0x04
#define VM_VM_AREA	0x04

// ARM10C 20141025
// ARM10C 20160820
// ARM10C 20160827
static DEFINE_SPINLOCK(vmap_area_lock);
/* Export for kexec only */
// ARM10C 20141108
LIST_HEAD(vmap_area_list);
// ARM10C 20140809
// ARM10C 20141025
// ARM10C 20141108
// ARM10C 20160820
// RB_ROOT: (struct rb_root) { NULL, }
static struct rb_root vmap_area_root = RB_ROOT;

/* The vmap cache globals are protected by vmap_area_lock */
// ARM10C 20141025
// ARM10C 20141206
// ARM10C 20150321
static struct rb_node *free_vmap_cache;
// ARM10C 20141025
// ARM10C 20141206
static unsigned long cached_hole_size;
// ARM10C 20141025
// ARM10C 20141206
static unsigned long cached_vstart;
// ARM10C 20141025
// ARM10C 20141206
static unsigned long cached_align;

// ARM10C 20140809
static unsigned long vmap_area_pcpu_hole;

// ARM10C 20160820
// addr: 할당받은 page의 mmu에 반영된 가상주소
static struct vmap_area *__find_vmap_area(unsigned long addr)
{
	struct rb_node *n = vmap_area_root.rb_node;
	// n: vmap_area_root.rb_node

	// n: vmap_area_root.rb_node
	while (n) {
		struct vmap_area *va;

		// n: vmap_area_root.rb_node: rb tree의 root (CHID)
		va = rb_entry(n, struct vmap_area, rb_node);
		// va : rb tree의 root (CHID) 의 struct vmap_area 의 시작 주소

		// addr: 할당받은 page의 mmu에 반영된 가상주소, va->va_start: rb tree의 root (CHID) 의 가상주소
		if (addr < va->va_start)
			n = n->rb_left;
		else if (addr >= va->va_end)
			n = n->rb_right;
		else
			return va;
	}

	// 위 loop에서 한일:
	// vmap_area_root.rb_node 에서 가지고 있는 rb tree의 주소를 기준으로
	// 할당받은 page의 mmu에 반영된 가상주소의 vmap_area 의 위치를 찾음

	return NULL;
}

// ARM10C 20140809
// va: kmem_cache#30-o9
// ARM10C 20141025
// va: kmem_cache#30-oX (GIC#0)
// ARM10C 20141108
// va: kmem_cache#30-oX (GIC#1)
// ARM10C 20141206
// va: kmem_cache#30-oX (COMB)
// ARM10C 20150110
// va: kmem_cache#30-oX (CLK)
// ARM10C 20150321
// va: kmem_cache#30-oX (MCT)
static void __insert_vmap_area(struct vmap_area *va)
{
	struct rb_node **p = &vmap_area_root.rb_node;
	// p: &vmap_area_root.rb_node
	// p: &vmap_area_root.rb_node
	// p: &vmap_area_root.rb_node
	// p: &vmap_area_root.rb_node
	// p: &vmap_area_root.rb_node
	// p: &vmap_area_root.rb_node
	struct rb_node *parent = NULL;
	// parent: NULL
	// parent: NULL
	// parent: NULL
	// parent: NULL
	// parent: NULL
	// parent: NULL
	struct rb_node *tmp;

	// *p: vmap_area_root.rb_node: NULL
	// *p: vmap_area_root.rb_node: CHID node
	// *p: vmap_area_root.rb_node: CHID node
	// *p: vmap_area_root.rb_node: CHID node
	// *p: vmap_area_root.rb_node: CHID node
	// *p: vmap_area_root.rb_node: CHID node
	while (*p) {
		struct vmap_area *tmp_va;

		// *p: vmap_area_root.rb_node: CHID node
		parent = *p;
		// parent: CHID node

		// parent: CHID node
		// rb_entry(CHID node, struct vmap_area, rb_node):
		// CHID 의 vmap_area 시작주소
		tmp_va = rb_entry(parent, struct vmap_area, rb_node);
		// tmp_va: CHID 의 vmap_area 시작주소

		// va->va_start: (kmem_cache#30-oX (GIC))->va_start: 0xf0000000,
		// tmp_va->va_end: (CHID 의 vmap_area 시작주소)->va_end: 0xf8001000
		if (va->va_start < tmp_va->va_end)
			// &(*p)->rb_left: &(CHID node)->rb_left
			p = &(*p)->rb_left;
			// p: TMR node
		else if (va->va_end > tmp_va->va_start)
			p = &(*p)->rb_right;
		else
			BUG();

		// GIC#0 node를 추가 할때 까지 루프 수행
		// GIC#1 node를 추가 할때 까지 루프 수행
		// COMB node를 추가 할때 까지 루프 수행
		// CLK node를 추가 할때 까지 루프 수행
		// MCT node를 추가 할때 까지 루프 수행
	}

	// while 수행 결과 rbtree를 순회 하여 GIC#0 node를 rbtree에 추가함
	/*
	// 가상주소 va_start 기준으로 GIC#0 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                 SYSC-b      WDT-b         CMU-b         SRAM-b
	//            (0xF6100000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /                                                 \
	//        GIC#0-r                                                 ROMC-r
	//    (0xF0000000)                                                (0xF84C0000)
	//
	*/

	// while 수행 결과 rbtree를 순회 하여 GIC#1 node를 rbtree에 추가함
	/*
	// 가상주소 va_start 기준으로 GIC#0 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                SYSC-b       WDT-b         CMU-b         SRAM-b
	//            (0xF6100000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /                                                  \
	//        GIC#0-r                                                  ROMC-r
	//    (0xF0000000)                                                (0xF84C0000)
	//             \
	//            GIC#1-r
	//          (0xF0002000)
	*/

	// while 수행 결과 rbtree를 순회 하여 COMB node를 rbtree에 추가함
	/*
	// 가상주소 va_start 기준으로 COMB 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-b      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-r     SYSC-r                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//                   /
	//               COMB-r
	//          (0xF0004000)
	*/

	// while 수행 결과 rbtree를 순회 하여 CLK node를 rbtree에 추가함
	/*
	// 가상주소 va_start 기준으로 CLK 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     SYSC-b                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//                   /
	//               COMB-r
	//          (0xF0004000)
	//                    \
	//                    CLK-r
	//                    (0xF0040000)
	*/

	// while 수행 결과 rbtree를 순회 하여 MCT node를 rbtree에 추가함
	/*
	// 가상주소 va_start 기준으로 MCT 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     CLK-b                                        ROMC-r
	//    (0xF0000000)   (0xF0040000)                                 (0xF84C0000)
	//                   /      \
	//               COMB-r     SYSC-r
	//          (0xF0004000)   (0xF6100000)
	//                     \
	//                      MCT-r
	//                 (0xF0006000)
	*/

	// va->rb_node: (kmem_cache#30-o9)->rb_node, parent: NULL, p: &vmap_area_root.rb_node
	// va->rb_node: (kmem_cache#30-oX (GIC#0))->rb_node, parent: SYSC node, p: (SYSC node)->rb_left
	// va->rb_node: (kmem_cache#30-oX (GIC#1))->rb_node, parent: GIG#0 node, p: (GIG#0 node)->rb_right
	// va->rb_node: (kmem_cache#30-oX (COMB))->rb_node, parent: SYSC node, p: (SYSC node)->rb_left
	// va->rb_node: (kmem_cache#30-oX (CLK))->rb_node, parent: COMB node, p: (COMB node)->rb_right
	// va->rb_node: (kmem_cache#30-oX (MCT))->rb_node, parent: COMB node, p: (COMB node)->rb_right
	rb_link_node(&va->rb_node, parent, p);
	// vmap_area_root.rb_node: &(kmem_cache#30-oX)->rb_node
	// (SYSC node)->rb_left: &(GIC#0)->rb_node
	// vmap_area_root.rb_node: &(kmem_cache#30-oX)->rb_node
	// (GIG#0 node)->rb_right: &(GIC#1)->rb_node
	// vmap_area_root.rb_node: &(kmem_cache#30-oX)->rb_node
	// (SYSC node)->rb_left: &(COMB)->rb_node
	// vmap_area_root.rb_node: &(kmem_cache#30-oX)->rb_node
	// (COMB node)->rb_right: &(CLK)->rb_node
	// vmap_area_root.rb_node: &(kmem_cache#30-oX)->rb_node
	// (COMB node)->rb_right: &(MCT)->rb_node

	// va->rb_node: (kmem_cache#30-oX)->rb_node
	// va->rb_node: (kmem_cache#30-oX (GIC#0))->rb_node
	// va->rb_node: (kmem_cache#30-oX (GIC#1))->rb_node
	// va->rb_node: (kmem_cache#30-oX (COMB))->rb_node
	// va->rb_node: (kmem_cache#30-oX (CLK))->rb_node
	// va->rb_node: (kmem_cache#30-oX (MCT))->rb_node
	rb_insert_color(&va->rb_node, &vmap_area_root);

	// rbtree 조건에 맞게 tree 구성 및 안정화 작업 수행
	/*
	// 가상주소 va_start 기준으로 GIC#0 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                 SYSC-b      WDT-b         CMU-b         SRAM-b
	//            (0xF6100000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /                                                 \
	//        GIC#0-r                                                 ROMC-r
	//    (0xF0000000)                                                (0xF84C0000)
	//
	*/

	// rbtree 조건에 맞게 tree 구성 및 안정화 작업 수행
	/*
	// 가상주소 va_start 기준으로 GIC#0 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-b      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-r     SYSC-r                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//
	*/

	// rbtree 조건에 맞게 tree 구성 및 안정화 작업 수행
	/*
	// 가상주소 va_start 기준으로 COMB 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     SYSC-b                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//                   /
	//               COMB-r
	//          (0xF0004000)
	*/

	// rbtree 조건에 맞게 tree 구성 및 안정화 작업 수행
	/*
	// 가상주소 va_start 기준으로 CLK 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     CLK-b                                        ROMC-r
	//    (0xF0000000)   (0xF0040000)                                 (0xF84C0000)
	//                   /      \
	//               COMB-r     SYSC-r
	//          (0xF0004000)   (0xF6100000)
	*/

	// rbtree 조건에 맞게 tree 구성 및 안정화 작업 수행
	/*
	// 가상주소 va_start 기준으로 MCT 를 RB Tree 추가한 결과
	//
	//                                      CHID-b
	//                                    (0xF8000000)
	//                                  /              \
	//                            CLK-b                  PMU-b
	//                         (0xF0040000)              (0xF8180000)
	//                        /          \                /        \
	//                 GIC#1-r            TMR-r        CMU-b         SRAM-b
	//             (0xF0002000)         (0xF6300000)   (0xF8100000)  (0xF8400000)
	//              /       \              /    \                         \
	//        GIC#0-b       COMB-b     SYSC-b     WDT-b                   ROMC-r
	//    (0xF0000000) (0xF0040000) (0xF6100000)  (0xF6400000)            (0xF84C0000)
	//                          \
	//                          MCT-r
	//                       (0xF0006000)
	*/

	/* address-sort this list */
	// va->rb_node: (kmem_cache#30-oX (GIC#0))->rb_node
	// rb_prev((kmem_cache#30-oX (GIC#0))->rb_node): NULL
	// va->rb_node: (kmem_cache#30-oX (GIC#1))->rb_node
	// rb_prev((kmem_cache#30-oX (GIC#1))->rb_node): (GIC#0)->rb_node
	// va->rb_node: (kmem_cache#30-oX (COMB))->rb_node
	// rb_prev((kmem_cache#30-oX (COMB))->rb_node): (GIC#1)->rb_node
	// va->rb_node: (kmem_cache#30-oX (CLK))->rb_node
	// rb_prev((kmem_cache#30-oX (CLK))->rb_node): (COMB)->rb_node
	// va->rb_node: (kmem_cache#30-oX (MCT))->rb_node
	// rb_prev((kmem_cache#30-oX (MCT))->rb_node): (COMB)->rb_node
	tmp = rb_prev(&va->rb_node);
	// tmp: NULL
	// tmp: (GIC#0)->rb_node
	// tmp: (GIC#1)->rb_node
	// tmp: (GOMB)->rb_node
	// tmp: (COMB)->rb_node

	// tmp: NULL
	// tmp: (GIC#0)->rb_node
	// tmp: (GIC#1)->rb_node
	// tmp: (GOMB)->rb_node
	// tmp: (COMB)->rb_node
	if (tmp) {
		struct vmap_area *prev;

		// tmp: (GIC#0)->rb_node
		// rb_entry((GIC#0)->rb_node, struct vmap_area, rb_node): GIC#0의 vmap_area의 시작주소
		// tmp: (GIC#1)->rb_node
		// rb_entry((GIC#1)->rb_node, struct vmap_area, rb_node): GIC#0의 vmap_area의 시작주소
		// tmp: (GOMB)->rb_node
		// rb_entry((GOMB)->rb_node, struct vmap_area, rb_node): GOMB의 vmap_area의 시작주소
		// tmp: (COMB)->rb_node
		// rb_entry((COMB)->rb_node, struct vmap_area, rb_node): COMB의 vmap_area의 시작주소
		prev = rb_entry(tmp, struct vmap_area, rb_node);
		// prev: GIC#0의 vmap_area의 시작주소
		// prev: GIC#1의 vmap_area의 시작주소
		// prev: COMB의 vmap_area의 시작주소
		// prev: COMB의 vmap_area의 시작주소

		// &va->list: &(kmem_cache#30-oX (GIC#1))->list, &prev->list: &(GIC#0)->list
		// &va->list: &(kmem_cache#30-oX (COMB))->list, &prev->list: &(GIC#1)->list
		// &va->list: &(kmem_cache#30-oX (CLK))->list, &prev->list: &(COMB)->list
		// &va->list: &(kmem_cache#30-oX (MCT))->list, &prev->list: &(COMB)->list
		list_add_rcu(&va->list, &prev->list);

		// list_add_rcu에서 한일:
		// ((GIC#1)->list)->next: (SYSC)->list
		// ((GIC#1)->list)->prev: (GIC#0)->list
		// core간 write memory barrier 수행
		// ((*((struct list_head __rcu **)(&(&(GIC#0)->list)->next)))) =
		// (typeof(*&((GIC#1))->list) __force space *)(&((GIC#1))->list)
		// ((SYSC)->list)->prev: &(GIC#1)->list

		// list_add_rcu에서 한일:
		// ((COMB)->list)->next: (SYSC)->list
		// ((COMB)->list)->prev: (GIC#0)->list
		// core간 write memory barrier 수행
		// ((*((struct list_head __rcu **)(&(&(GIC#1)->list)->next)))) =
		// (typeof(*&((COMB))->list) __force space *)(&((COMB))->list)
		// ((SYSC)->list)->prev: &(COMB)->list

		// list_add_rcu에서 한일:
		// ((CLK)->list)->next: (SYSC)->list
		// ((CLK)->list)->prev: (COMB)->list
		// core간 write memory barrier 수행
		// ((*((struct list_head __rcu **)(&(&(COMB)->list)->next)))) =
		// (typeof(*&((CLK))->list) __force space *)(&((CLK))->list)
		// ((SYSC)->list)->prev: &(CLK)->list

		// list_add_rcu에서 한일:
		// ((MCT)->list)->next: (CLK)->list
		// ((MCT)->list)->prev: (COMB)->list
		// core간 write memory barrier 수행
		// ((*((struct list_head __rcu **)(&(&(COMB)->list)->next)))) =
		// (typeof(*&((MCT))->list) __force space *)(&((MCT))->list)
		// ((CLK)->list)->prev: &(MCT)->list
	} else
		// &va->list: &(kmem_cache#30-oX (GIC))->list
		list_add_rcu(&va->list, &vmap_area_list);
		// list_add_rcu에서 한일:
		// ((GIC)->list)->next: (SYSC)->list
		// ((GIC)->list)->prev: &vmap_area_list
		// core간 write memory barrier 수행
		// ((*((struct list_head __rcu **)(&(&vmap_area_list)->next)))) =
		// (typeof(*&((GIC))->list) __force space *)(&((GIC))->list)
		// ((SYSC)->list)->prev: &(GIC)->list
}

static void purge_vmap_area_lazy(void);

/*
 * Allocate a region of KVA of the specified size and alignment, within the
 * vstart and vend.
 */
// ARM10C 20141025
// [1st] size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
// ARM10C 20141108
// [2nd] size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
// ARM10C 20141206
// [3rd] size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
// ARM10C 20150110
// [4th] size: 0x31000, align: 0x40000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
// ARM10C 20150321
// [5th] size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
static struct vmap_area *alloc_vmap_area(unsigned long size,
				unsigned long align,
				unsigned long vstart, unsigned long vend,
				int node, gfp_t gfp_mask)
{
	struct vmap_area *va;
	struct rb_node *n;
	unsigned long addr;
	int purged = 0;
	// [1st] purged: 0
	// [2nd] purged: 0
	// [3rd] purged: 0
	// [4th] purged: 0
	// [5th] purged: 0
	struct vmap_area *first;

	// [1st] size: 0x2000
	// [2nd] size: 0x2000
	// [3rd] size: 0x2000
	// [4th] size: 0x31000
	// [5th] size: 0x2000
	BUG_ON(!size);

	// [1st] size: 0x2000, PAGE_MASK: 0xFFFFF000
	// [2nd] size: 0x2000, PAGE_MASK: 0xFFFFF000
	// [3rd] size: 0x2000, PAGE_MASK: 0xFFFFF000
	// [4th] size: 0x31000, PAGE_MASK: 0xFFFFF000
	// [5th] size: 0x2000, PAGE_MASK: 0xFFFFF000
	BUG_ON(size & ~PAGE_MASK);

	// [1st] align: 0x2000, is_power_of_2(0x2000): 1
	// [2nd] align: 0x2000, is_power_of_2(0x2000): 1
	// [3rd] align: 0x2000, is_power_of_2(0x2000): 1
	// [4th] align: 0x40000, is_power_of_2(0x40000): 1
	// [5th] align: 0x2000, is_power_of_2(0x2000): 1
	BUG_ON(!is_power_of_2(align));

	// [1st] sizeof(struct vmap_area): 52 bytes, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// [1st] kmalloc_node(52, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX
	// [2nd] sizeof(struct vmap_area): 52 bytes, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// [2nd] kmalloc_node(52, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX
	// [3rd] sizeof(struct vmap_area): 52 bytes, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// [3rd] kmalloc_node(52, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX
	// [4th] sizeof(struct vmap_area): 52 bytes, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// [4th] kmalloc_node(52, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX
	// [5th] sizeof(struct vmap_area): 52 bytes, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// [5th] kmalloc_node(52, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX
	va = kmalloc_node(sizeof(struct vmap_area),
			gfp_mask & GFP_RECLAIM_MASK, node);
	// [1st] va: kmem_cache#30-oX
	// [2nd] va: kmem_cache#30-oX
	// [3rd] va: kmem_cache#30-oX
	// [4th] va: kmem_cache#30-oX
	// [5th] va: kmem_cache#30-oX

	// [1st] va: kmem_cache#30-oX
	// [2nd] va: kmem_cache#30-oX
	// [3rd] va: kmem_cache#30-oX
	// [4th] va: kmem_cache#30-oX
	// [5th] va: kmem_cache#30-oX
	if (unlikely(!va))
		return ERR_PTR(-ENOMEM);

	/*
	 * Only scan the relevant parts containing pointers to other objects
	 * to avoid false negatives.
	 */
	// [1st] &va->rb_node: &(kmem_cache#30-oX)->rb_node, SIZE_MAX: 0xFFFFFFFF, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0
	// [2nd] &va->rb_node: &(kmem_cache#30-oX)->rb_node, SIZE_MAX: 0xFFFFFFFF, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0
	// [3rd] &va->rb_node: &(kmem_cache#30-oX)->rb_node, SIZE_MAX: 0xFFFFFFFF, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0
	// [4th] &va->rb_node: &(kmem_cache#30-oX)->rb_node, SIZE_MAX: 0xFFFFFFFF, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0
	// [5th] &va->rb_node: &(kmem_cache#30-oX)->rb_node, SIZE_MAX: 0xFFFFFFFF, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0
	kmemleak_scan_area(&va->rb_node, SIZE_MAX, gfp_mask & GFP_RECLAIM_MASK); // null function

retry:
	spin_lock(&vmap_area_lock);
	// [1st] vmap_area_lock을 이용한 spinlock 설정 수행
	// [2nd] vmap_area_lock을 이용한 spinlock 설정 수행
	// [3rd] vmap_area_lock을 이용한 spinlock 설정 수행
	// [4th] vmap_area_lock을 이용한 spinlock 설정 수행
	// [5th] vmap_area_lock을 이용한 spinlock 설정 수행

	/*
	 * Invalidate cache if we have more permissive parameters.
	 * cached_hole_size notes the largest hole noticed _below_
	 * the vmap_area cached in free_vmap_cache: if size fits
	 * into that hole, we want to scan from vstart to reuse
	 * the hole instead of allocating above free_vmap_cache.
	 * Note that __free_vmap_area may update free_vmap_cache
	 * without updating cached_hole_size or cached_align.
	 */
	// [1st] free_vmap_cache: NULL, size: 0x2000, cached_hole_size: 0
	// [1st] vstart: 0xf0000000, cached_vstart: 0, align: 0x2000, cached_align: 0
	// [2nd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#0), size: 0x2000, cached_hole_size: 0
	// [2nd] vstart: 0xf0000000, cached_vstart: 0xf0000000, align: 0x2000, cached_align: 0x2000
	// [3rd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#1), size: 0x2000, cached_hole_size: 0
	// [3rd] vstart: 0xf0000000, cached_vstart: 0xf0000000, align: 0x2000, cached_align: 0x2000
	// [4th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (COMB), size: 0x31000, cached_hole_size: 0
	// [4th] vstart: 0xf0000000, cached_vstart: 0xf0000000, align: 0x40000, cached_align: 0x2000
	// [5th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (CLK), size: 0x2000, cached_hole_size: 0
	// [5th] vstart: 0xf0000000, cached_vstart: 0xf0000000, align: 0x2000, cached_align: 0x40000
	if (!free_vmap_cache ||
			size < cached_hole_size ||
			vstart < cached_vstart ||
			align < cached_align) {
nocache:
		// [1st] cached_hole_size: 0
		// [5th] cached_hole_size: 0
		cached_hole_size = 0;
		// [1st] cached_hole_size: 0
		// [5th] cached_hole_size: 0

		// [1st] free_vmap_cache: NULL
		// [5th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (CLK)
		free_vmap_cache = NULL;
		// [1st] free_vmap_cache: NULL
		// [5th] free_vmap_cache: NULL
	}

	/* record if we encounter less permissive parameters */
	// [1st] cached_vstart: 0, vstart: 0xf0000000
	// [2nd] cached_vstart: 0xf0000000, vstart: 0xf0000000
	// [3rd] cached_vstart: 0xf0000000, vstart: 0xf0000000
	// [4th] cached_vstart: 0xf0000000, vstart: 0xf0000000
	// [5th] cached_vstart: 0xf0000000, vstart: 0xf0000000
	cached_vstart = vstart;
	// [1st] cached_vstart: 0xf0000000
	// [2nd] cached_vstart: 0xf0000000
	// [3rd] cached_vstart: 0xf0000000
	// [4th] cached_vstart: 0xf0000000
	// [5th] cached_vstart: 0xf0000000

	// [1st] cached_align: 0, align: 0x2000
	// [2nd] cached_align: 0x2000, align: 0x2000
	// [3rd] cached_align: 0x2000, align: 0x2000
	// [4th] cached_align: 0x2000, align: 0x40000
	// [5th] cached_align: 0x40000, align: 0x2000
	cached_align = align;
	// [1st] cached_align: 0x2000
	// [2nd] cached_align: 0x2000
	// [3rd] cached_align: 0x2000
	// [4th] cached_align: 0x40000
	// [5th] cached_align: 0x2000

	/* find starting point for our search */
	// [1st] free_vmap_cache: NULL
	// [2nd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#0)
	// [3rd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#1)
	// [4th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (COMB)
	// [5th] free_vmap_cache: NULL
	if (free_vmap_cache) {
		// [2nd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#0)
		// [2nd] rb_entry(&(kmem_cache#30-oX)->rb_node (GIC#0), struct vmap_area, rb_node):
		// [2nd] kmem_cache#30-oX (GIC#0)
		// [3rd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#1)
		// [3rd] rb_entry(&(kmem_cache#30-oX)->rb_node (GIC#1), struct vmap_area, rb_node):
		// [3rd] kmem_cache#30-oX (GIC#1)
		// [4th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (COMB)
		// [4th] rb_entry(&(kmem_cache#30-oX)->rb_node (COMB), struct vmap_area, rb_node):
		// [4th] kmem_cache#30-oX (COMB)
		first = rb_entry(free_vmap_cache, struct vmap_area, rb_node);
		// [2nd] first: kmem_cache#30-oX (GIC#0)
		// [3rd] first: kmem_cache#30-oX (GIC#1)
		// [4th] first: kmem_cache#30-oX (COMB)

		// [2nd] first->va_end: (kmem_cache#30-oX (GIC#0))->va_end: 0xf0002000, align: 0x2000
		// [2nd] ALIGN(0xf0002000, 0x2000): 0xf0002000
		// [3rd] first->va_end: (kmem_cache#30-oX (GIC#1))->va_end: 0xf0004000, align: 0x2000
		// [3rd] ALIGN(0xf0004000, 0x2000): 0xf0004000
		// [4th] first->va_end: (kmem_cache#30-oX (COMB))->va_end: 0xf0006000, align: 0x40000
		// [4th] ALIGN(0xf0006000, 0x40000): 0xf0040000
		addr = ALIGN(first->va_end, align);
		// [2nd] addr: 0xf0002000
		// [3rd] addr: 0xf0004000
		// [4th] addr: 0xf0040000

		// [2nd] addr: 0xf0002000, vstart: 0xf0000000
		// [3rd] addr: 0xf0004000, vstart: 0xf0000000
		// [4th] addr: 0xf0040000, vstart: 0xf0000000
		if (addr < vstart)
			goto nocache;

		// [2nd] addr: 0xf0002000, size: 0x2000
		// [3rd] addr: 0xf0004000, size: 0x2000
		// [4th] addr: 0xf0040000, size: 0x31000
		if (addr + size < addr)
			goto overflow;

	} else {
		// [1st] vstart: 0xf0000000, 0x2000, ALIGN(0xf0000000, 0x2000): 0xf0000000
		// [5th] vstart: 0xf0000000, 0x2000, ALIGN(0xf0000000, 0x2000): 0xf0000000
		addr = ALIGN(vstart, align);
		// [1st] addr: 0xf0000000
		// [5th] addr: 0xf0000000

		// [1st] addr: 0xf0000000, size: 0x2000
		// [5th] addr: 0xf0000000, size: 0x2000
		if (addr + size < addr)
			goto overflow;

		/*
		// NOTE: [1st]
		// 가상주소 va_start 기준으로 RB Tree 구성한 결과
		//
		//                          CHID-b
		//                       (0xF8000000)
		//                      /            \
		//                 TMR-r               PMU-r
		//            (0xF6300000)             (0xF8180000)
		//              /      \               /           \
		//         SYSC-b      WDT-b         CMU-b         SRAM-b
		//    (0xF6100000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
		//                                                       \
		//                                                        ROMC-r
		//                                                        (0xF84C0000)
		//
		// vmap_area_root.rb_node: CHID rb_node
		*/

		/*
		// NOTE: [5th]
		// 가상주소 va_start 기준으로 RB Tree 구성한 결과
		//
		//                                  CHID-b
		//                               (0xF8000000)
		//                              /            \
		//                         TMR-b               PMU-b
		//                    (0xF6300000)             (0xF8180000)
		//                      /      \               /           \
		//                GIC#1-r      WDT-b         CMU-b         SRAM-b
		//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
		//             /       \                                          \
		//        GIC#0-b     CLK-b                                        ROMC-r
		//    (0xF0000000)   (0xF0040000)                                 (0xF84C0000)
		//                   /      \
		//               COMB-r     SYSC-r
		//          (0xF0004000)   (0xF6100000)
		//
		// vmap_area_root.rb_node: CHID rb_node
		*/

		// [1st] vmap_area_root.rb_node: CHID rb_node
		// [5th] vmap_area_root.rb_node: CHID rb_node
		n = vmap_area_root.rb_node;
		// [1st] n: CHID rb_node
		// [5th] n: CHID rb_node

		first = NULL;
		// [1st] first: NULL
		// [5th] first: NULL

		// [1st] n: CHID rb_node
		// [5th] n: CHID rb_node
		while (n) {
			struct vmap_area *tmp;

			// NOTE:
			// while 의 수행 횟수를 [w1]...[wN] 으로 주석의 prefix에 추가

			// [1st][w1] n: CHID rb_node, rb_entry(CHID rb_node, struct vmap_area, rb_node): CHID vmap_area 의 시작주소
			// [1st][w2] n: TMR rb_node, rb_entry(TMR rb_node, struct vmap_area, rb_node): TMR vmap_area 의 시작주소
			// [1st][w3] n: SYSC rb_node, rb_entry(SYSC rb_node, struct vmap_area, rb_node): SYSC vmap_area 의 시작주소
			// [5th][w1] n: CHID rb_node, rb_entry(CHID rb_node, struct vmap_area, rb_node): CHID vmap_area 의 시작주소
			// [5th][w2] n: TMR rb_node, rb_entry(TMR rb_node, struct vmap_area, rb_node): TMR vmap_area 의 시작주소
			// [5th][w3] n: GIC#1 rb_node, rb_entry(GIC#1 rb_node, struct vmap_area, rb_node): GIC#1 vmap_area 의 시작주소
			// [5th][w4] n: GIC#0 rb_node, rb_entry(GIC#0 rb_node, struct vmap_area, rb_node): GIC#0 vmap_area 의 시작주소
			tmp = rb_entry(n, struct vmap_area, rb_node);
			// [1st][w1] tmp: CHID vmap_area 의 시작주소
			// [1st][w2] tmp: TMR vmap_area 의 시작주소
			// [1st][w3] tmp: SYSC vmap_area 의 시작주소
			// [5th][w1] tmp: CHID vmap_area 의 시작주소
			// [5th][w2] tmp: TMR vmap_area 의 시작주소
			// [5th][w3] tmp: GIC#1 vmap_area 의 시작주소
			// [5th][w4] tmp: GIC#0 vmap_area 의 시작주소

			// [1st][w1] CHID vmap_area의 맴버값
			// [1st][w1] va->va_start: 0xf8000000, va->va_end: 0xf8001000
			// [1st][w2] TMR vmap_area의 맴버값
			// [1st][w2] va->va_start: 0xf6300000, va->va_end: 0xf6304000
			// [1st][w3] SYSC vmap_area의 맴버값
			// [1st][w3] va->va_start: 0xf6100000, va->va_end: 0xf6110000

			// [5th][w1] CHID vmap_area의 맴버값
			// [5th][w1] va->va_start: 0xf8000000, va->va_end: 0xf8001000
			// [5th][w2] TMR vmap_area의 맴버값
			// [5th][w2] va->va_start: 0xf6300000, va->va_end: 0xf6304000
			// [5th][w3] GIC#1 vmap_area의 맴버값
			// [5th][w3] va->va_start: 0xf0002000, va->va_end: 0xf0004000
			// [5th][w4] GIC#0 vmap_area의 맴버값
			// [5th][w4] va->va_start: 0xf0000000, va->va_end: 0xf0002000

			// [1st][w1] tmp->va_end: (CHID)->va_end: 0xf8001000, addr: 0xf0000000
			// [1st][w2] tmp->va_end: (TMR)->va_end: 0xf6304000, addr: 0xf0000000
			// [1st][w3] tmp->va_end: (SYSC)->va_end: 0xf6110000, addr: 0xf0000000
			// [5th][w1] tmp->va_end: (CHID)->va_end: 0xf8001000, addr: 0xf0000000
			// [5th][w2] tmp->va_end: (TMR)->va_end: 0xf6304000, addr: 0xf0000000
			// [5th][w3] tmp->va_end: (GIC#1)->va_end: 0xf0004000, addr: 0xf0000000
			// [5th][w4] tmp->va_end: (GIC#0)->va_end: 0xf0002000, addr: 0xf0000000
			if (tmp->va_end >= addr) {
				// [1st][w1] tmp: CHID vmap_area 의 시작주소
				// [1st][w2] tmp: TMR vmap_area 의 시작주소
				// [1st][w3] tmp: SYSC vmap_area 의 시작주소
				// [5th][w1] tmp: CHID vmap_area 의 시작주소
				// [5th][w2] tmp: TMR vmap_area 의 시작주소
				// [5th][w3] tmp: GIC#1 vmap_area 의 시작주소
				// [5th][w4] tmp: GIC#0 vmap_area 의 시작주소
				first = tmp;
				// [1st][w1] first: CHID vmap_area 의 시작주소
				// [1st][w2] first: TMR vmap_area 의 시작주소
				// [1st][w3] first: SYSC vmap_area 의 시작주소
				// [5th][w1] first: CHID vmap_area 의 시작주소
				// [5th][w2] first: TMR vmap_area 의 시작주소
				// [5th][w3] first: GIC#1 vmap_area 의 시작주소
				// [5th][w4] first: GIC#0 vmap_area 의 시작주소

				// [1st][w1] tmp->va_start: (CHID)->va_start: 0xf8000000, addr: 0xf0000000
				// [1st][w2] tmp->va_start: (TMR)->va_start: 0xf6300000, addr: 0xf0000000
				// [1st][w3] tmp->va_start: (SYSC)->va_start: 0xf6110000, addr: 0xf0000000
				// [5th][w1] tmp->va_start: (CHID)->va_start: 0xf8000000, addr: 0xf0000000
				// [5th][w2] tmp->va_start: (TMR)->va_start: 0xf6300000, addr: 0xf0000000
				// [5th][w3] tmp->va_start: (GIC#1)->va_start: 0xf0002000, addr: 0xf0000000
				// [5th][w4] tmp->va_start: (GIC#0)->va_start: 0xf0000000, addr: 0xf0000000
				if (tmp->va_start <= addr)
					break;
					// [5th][w4] break 수행

				// [1st][w1] n->rb_left: (CHID rb_node)->rb_left: TMR rb_node
				// [1st][w2] n->rb_left: (TMR rb_node)->rb_left: SYSC rb_node
				// [1st][w3] n->rb_left: (SYSC rb_node)->rb_left: NULL
				// [5th][w1] n->rb_left: (CHID rb_node)->rb_left: TMR rb_node
				// [5th][w2] n->rb_left: (TMR rb_node)->rb_left: GIC#1 rb_node
				// [5th][w3] n->rb_left: (GIC#1 rb_node)->rb_left: GIC#0 rb_node
				n = n->rb_left;
				// [1st][w1] n: TMR rb_node
				// [1st][w2] n: SYSC rb_node
				// [1st][w3] n: NULL
				// [5th][w1] n: TMR rb_node
				// [5th][w2] n: GIC#1 rb_node
				// [5th][w3] n: GIC#0 rb_node
			} else
				n = n->rb_right;
		}

		// [1st] 위 loop 수행 결과
		// [1st] first: SYSC vmap_area 의 시작주소

		// [5th] 위 loop 수행 결과
		// [5th] first: GIC#0 vmap_area 의 시작주소

		// [1st] first: SYSC vmap_area 의 시작주소
		// [5th] first: GIC#0 vmap_area 의 시작주소
		if (!first)
			goto found;
	}

	/* from the starting point, walk areas until a suitable hole is found */
	// [1st] addr: 0xf0000000, size: 0x2000, first->va_start: (SYSC)->va_start: 0xf6100000, vend: 0xff000000
	// [2nd] addr: 0xf0002000, size: 0x2000, first->va_start: (GIC#0)->va_start: 0xf0000000, vend: 0xff000000
	// [3rd] addr: 0xf0004000, size: 0x2000, first->va_start: (GIC#1)->va_start: 0xf0002000, vend: 0xff000000
	// [4th] addr: 0xf0040000, size: 0x31000, first->va_start: (COMB)->va_start: 0xf0004000, vend: 0xff000000
	// [5th] addr: 0xf0000000, size: 0x2000, first->va_start: (GIC#0)->va_start: 0xf0000000, vend: 0xff000000
	while (addr + size > first->va_start && addr + size <= vend) {
		// NOTE:
		// while 의 수행 횟수를 [w1]...[wN] 으로 주석의 prefix에 추가

		// [2nd] addr: 0xf0002000, cached_hole_size: 0, first->va_start: (GIC#0)->va_start: 0xf0000000
		// [3rd] addr: 0xf0004000, cached_hole_size: 0, first->va_start: (GIC#1)->va_start: 0xf0002000
		// [4th] addr: 0xf0040000, cached_hole_size: 0, first->va_start: (COMB)->va_start: 0xf0004000
		// [5th][w1] addr: 0xf0000000, cached_hole_size: 0, first->va_start: (GIC#0)->va_start: 0xf0000000
		// [5th][w2] addr: 0xf0002000, cached_hole_size: 0, first->va_start: (GIC#1)->va_start: 0xf0002000
		// [5th][w3] addr: 0xf0004000, cached_hole_size: 0, first->va_start: (COMB)->va_start: 0xf0004000
		if (addr + cached_hole_size < first->va_start)
			cached_hole_size = first->va_start - addr;

		// [2nd] first->va_end: (GIC#0)->va_end: 0xf0002000, align: 0x2000
		// [3rd] first->va_end: (GIC#1)->va_end: 0xf0004000, align: 0x2000
		// [4th] first->va_end: (COMB)->va_end: 0xf0006000, align: 0x40000
		// [5th][w1] first->va_end: (GIC#0)->va_end: 0xf0002000, align: 0x2000
		// [5th][w2] first->va_end: (GIC#1)->va_end: 0xf0004000, align: 0x2000
		// [5th][w3] first->va_end: (COMB)->va_end: 0xf0006000, align: 0x2000
		addr = ALIGN(first->va_end, align);
		// [2nd] addr: 0xf0002000
		// [3rd] addr: 0xf0004000
		// [4th] addr: 0xf0040000
		// [5th][w1] addr: 0xf0002000
		// [5th][w2] addr: 0xf0004000
		// [5th][w3] addr: 0xf0006000

		// [2nd] addr: 0xf0002000, size: 0x2000
		// [3rd] addr: 0xf0004000, size: 0x2000
		// [4th] addr: 0xf0040000, size: 0x31000
		// [5th][w1] addr: 0xf0002000, size: 0x2000
		// [5th][w2] addr: 0xf0004000, size: 0x2000
		// [5th][w3] addr: 0xf0006000, size: 0x2000
		if (addr + size < addr)
			goto overflow;

		// [2nd] &first->list: &(GIC#0)->list
		// [2nd] list_is_last(&(GIC#0)->list, &vmap_area_list): 0
		// [3rd] &first->list: &(GIC#1)->list
		// [3rd] list_is_last(&(GIC#1)->list, &vmap_area_list): 0
		// [4th] &first->list: &(COMB)->list
		// [4th] list_is_last(&(COMB)->list, &vmap_area_list): 0
		// [5th][w1] &first->list: &(GIC#0)->list
		// [5th][w1] list_is_last(&(GIC#0)->list, &vmap_area_list): 0
		// [5th][w2] &first->list: &(GIC#1)->list
		// [5th][w2] list_is_last(&(GIC#1)->list, &vmap_area_list): 0
		// [5th][w3] &first->list: &(COMB)->list
		// [5th][w3] list_is_last(&(COMB)->list, &vmap_area_list): 0
		if (list_is_last(&first->list, &vmap_area_list))
			goto found;

		// [2nd] first->list.next: (GIC#0)->list.next: (SYSC)->list
		// [2nd] list_entry((SYSC)->list, struct vmap_area, list): SYSC의 vmap_area 시작주소
		// [3rd] first->list.next: (GIC#1)->list.next: (SYSC)->list
		// [3rd] list_entry((SYSC)->list, struct vmap_area, list): SYSC의 vmap_area 시작주소
		// [4th] first->list.next: (COMB)->list.next: (SYSC)->list
		// [4th] list_entry((SYSC)->list, struct vmap_area, list): SYSC의 vmap_area 시작주소
		// [5th][w1] first->list.next: (GIC#0)->list.next: (GIC#1)->list
		// [5th][w1] list_entry((GIC#1)->list, struct vmap_area, list): GIC#1의 vmap_area 시작주소
		// [5th][w2] first->list.next: (GIC#1)->list.next: (COMB)->list
		// [5th][w2] list_entry((COMB)->list, struct vmap_area, list): COMB의 vmap_area 시작주소
		// [5th][w3] first->list.next: (COMB)->list.next: (CLK)->list
		// [5th][w3] list_entry((CLK)->list, struct vmap_area, list): CLK의 vmap_area 시작주소
		first = list_entry(first->list.next,
				struct vmap_area, list);
		// [2nd] first: SYSC의 vmap_area 시작주소
		// [3rd] first: SYSC의 vmap_area 시작주소
		// [4th] first: SYSC의 vmap_area 시작주소
		// [5th][w1] first: GIC#1의 vmap_area 시작주소
		// [5th][w2] first: COMB의 vmap_area 시작주소
		// [5th][w3] first: CLK의 vmap_area 시작주소

		// [2nd] addr: 0xf0002000, size: 0x2000, first->va_start: (SYSC)->va_start: 0xf6100000, vend: 0xff000000
		// [3rd] addr: 0xf0004000, size: 0x2000, first->va_start: (SYSC)->va_start: 0xf6100000, vend: 0xff000000
		// [4th] addr: 0xf0040000, size: 0x31000, first->va_start: (SYSC)->va_start: 0xf6100000, vend: 0xff000000

		// [5th][w1] addr: 0xf0002000, size: 0x2000, first->va_start: (GIC#1)->va_start: 0xf0002000, vend: 0xff000000
		// [5th][w2] addr: 0xf0004000, size: 0x2000, first->va_start: (COMB)->va_start: 0xf0004000, vend: 0xff000000
		// [5th][w3] addr: 0xf0006000, size: 0x2000, first->va_start: (CLK)->va_start: 0xf0040000, vend: 0xff000000
	}

found:
	// [1st] addr: 0xf0000000, size: 0x2000, vend: 0xff000000
	// [2nd] addr: 0xf0002000, size: 0x2000, vend: 0xff000000
	// [3rd] addr: 0xf0004000, size: 0x2000, vend: 0xff000000
	// [4th] addr: 0xf0040000, size: 0x31000, vend: 0xff000000
	// [5th] addr: 0xf0006000, size: 0x2000, vend: 0xff000000
	if (addr + size > vend)
		goto overflow;

	// [1st] va->va_start: (kmem_cache#30-oX)->va_start, addr: 0xf0000000
	// [2nd] va->va_start: (kmem_cache#30-oX)->va_start, addr: 0xf0002000
	// [3rd] va->va_start: (kmem_cache#30-oX)->va_start, addr: 0xf0004000
	// [4th] va->va_start: (kmem_cache#30-oX)->va_start, addr: 0xf0040000
	// [5th] va->va_start: (kmem_cache#30-oX)->va_start, addr: 0xf0006000
	va->va_start = addr;
	// [1st] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0000000
	// [2nd] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0002000
	// [3rd] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0004000
	// [4th] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0040000
	// [5th] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0006000

	// [1st] va->va_end: (kmem_cache#30-oX)->va_end, addr: 0xf0000000, size: 0x2000
	// [2nd] va->va_end: (kmem_cache#30-oX)->va_end, addr: 0xf0002000, size: 0x2000
	// [3rd] va->va_end: (kmem_cache#30-oX)->va_end, addr: 0xf0004000, size: 0x2000
	// [4th] va->va_end: (kmem_cache#30-oX)->va_end, addr: 0xf0040000, size: 0x31000
	// [5th] va->va_end: (kmem_cache#30-oX)->va_end, addr: 0xf0006000, size: 0x2000
	va->va_end = addr + size;
	// [1st] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0002000
	// [2nd] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0004000
	// [3rd] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0006000
	// [4th] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0071000
	// [5th] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0008000

	// [1st] va->flags: (kmem_cache#30-oX)->flags
	// [2nd] va->flags: (kmem_cache#30-oX)->flags
	// [3rd] va->flags: (kmem_cache#30-oX)->flags
	// [4th] va->flags: (kmem_cache#30-oX)->flags
	// [5th] va->flags: (kmem_cache#30-oX)->flags
	va->flags = 0;
	// [1st] va->flags: (kmem_cache#30-oX)->flags: 0
	// [2nd] va->flags: (kmem_cache#30-oX)->flags: 0
	// [3rd] va->flags: (kmem_cache#30-oX)->flags: 0
	// [4th] va->flags: (kmem_cache#30-oX)->flags: 0
	// [5th] va->flags: (kmem_cache#30-oX)->flags: 0

	// [1st] va: kmem_cache#30-oX (GIC#0)
	// [2nd] va: kmem_cache#30-oX (GIC#1)
	// [3rd] va: kmem_cache#30-oX (COMB)
	// [4th] va: kmem_cache#30-oX (CLK)
	// [5th] va: kmem_cache#30-oX (MCT)
	__insert_vmap_area(va);

	/*
	// [1st]
	// 가상주소 va_start 기준으로 GIC#0 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                 SYSC-b      WDT-b         CMU-b         SRAM-b
	//            (0xF6100000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /                                                 \
	//        GIC#0-r                                                 ROMC-r
	//   (0xF0000000)                                                 (0xF84C0000)
	//
	// vmap_area_list에 GIC#0 - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// [2nd]
	// 가상주소 va_start 기준으로 GIC#1 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-b      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-r     SYSC-r                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// [3rd]
	// 가상주소 va_start 기준으로 COMB 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     SYSC-b                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//                   /
	//               COMB-r
	//          (0xF0004000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - COMB - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// [4th]
	// 가상주소 va_start 기준으로 CLK 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     CLK-b                                        ROMC-r
	//    (0xF0000000)   (0xF0040000)                                 (0xF84C0000)
	//                   /      \
	//               COMB-r     SYSC-r
	//          (0xF0004000)   (0xF6100000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - COMB - CLK - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// [5th]
	// 가상주소 va_start 기준으로 MCT 를 RB Tree 추가한 결과
	//
	//                                      CHID-b
	//                                    (0xF8000000)
	//                                  /              \
	//                            CLK-b                  PMU-b
	//                         (0xF0040000)              (0xF8180000)
	//                        /          \                /        \
	//                 GIC#1-r            TMR-r        CMU-b         SRAM-b
	//             (0xF0002000)         (0xF6300000)   (0xF8100000)  (0xF8400000)
	//              /       \              /    \                         \
	//        GIC#0-b       COMB-b     SYSC-b     WDT-b                   ROMC-r
	//    (0xF0000000) (0xF0004000) (0xF6100000)  (0xF6400000)            (0xF84C0000)
	//                          \
	//                          MCT-r
	//                       (0xF0006000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - COMB - MCT - CLK - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	// [1st] &va->rb_node: &(kmem_cache#30-oX)->rb_node (GIC#0)
	// [2nd] &va->rb_node: &(kmem_cache#30-oX)->rb_node (GIC#1)
	// [3rd] &va->rb_node: &(kmem_cache#30-oX)->rb_node (COMB)
	// [4th] &va->rb_node: &(kmem_cache#30-oX)->rb_node (CLK)
	// [5th] &va->rb_node: &(kmem_cache#30-oX)->rb_node (MCT)
	free_vmap_cache = &va->rb_node;
	// [1st] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#0)
	// [2nd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (GIC#1)
	// [3rd] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (COMB)
	// [4th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (CLK)
	// [5th] free_vmap_cache: &(kmem_cache#30-oX)->rb_node (MCT)

	spin_unlock(&vmap_area_lock);
	// [1st] vmap_area_lock을 이용한 spinlock 해재 수행
	// [2nd] vmap_area_lock을 이용한 spinlock 해재 수행
	// [3rd] vmap_area_lock을 이용한 spinlock 해재 수행
	// [4th] vmap_area_lock을 이용한 spinlock 해재 수행
	// [5th] vmap_area_lock을 이용한 spinlock 해재 수행

	// [1st] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0000000, align: 0x2000
	// [2nd] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0002000, align: 0x2000
	// [3rd] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0004000, align: 0x2000
	// [4th] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0040000, align: 0x40000
	// [5th] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0006000, align: 0x2000
	BUG_ON(va->va_start & (align-1));

	// [1st] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0000000, vstart: 0xf0000000
	// [2nd] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0002000, vstart: 0xf0000000
	// [3rd] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0004000, vstart: 0xf0000000
	// [4th] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0040000, vstart: 0xf0000000
	// [5th] va->va_start: (kmem_cache#30-oX)->va_start: 0xf0006000, vstart: 0xf0000000
	BUG_ON(va->va_start < vstart);

	// [1st] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0002000, vend: 0xff000000
	// [2nd] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0004000, vend: 0xff000000
	// [3rd] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0006000, vend: 0xff000000
	// [4th] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0071000, vend: 0xff000000
	// [5th] va->va_end: (kmem_cache#30-oX)->va_end: 0xf0008000, vend: 0xff000000
	BUG_ON(va->va_end > vend);

	// [1st] va: kmem_cache#30-oX (GIC#0)
	// [2nd] va: kmem_cache#30-oX (GIC#1)
	// [3rd] va: kmem_cache#30-oX (COMB)
	// [4th] va: kmem_cache#30-oX (CLK)
	// [5th] va: kmem_cache#30-oX (MCT)
	return va;
	// [1st] return kmem_cache#30-oX (GIC#0)
	// [2nd] return kmem_cache#30-oX (GIC#1)
	// [3rd] return kmem_cache#30-oX (COMB)
	// [4th] return kmem_cache#30-oX (CLK)
	// [5th] return kmem_cache#30-oX (MCT)

overflow:
	spin_unlock(&vmap_area_lock);
	if (!purged) {
		purge_vmap_area_lazy();
		purged = 1;
		goto retry;
	}
	if (printk_ratelimit())
		printk(KERN_WARNING
			"vmap allocation for size %lu failed: "
			"use vmalloc=<size> to increase size.\n", size);
	kfree(va);
	return ERR_PTR(-EBUSY);
}

static void __free_vmap_area(struct vmap_area *va)
{
	BUG_ON(RB_EMPTY_NODE(&va->rb_node));

	if (free_vmap_cache) {
		if (va->va_end < cached_vstart) {
			free_vmap_cache = NULL;
		} else {
			struct vmap_area *cache;
			cache = rb_entry(free_vmap_cache, struct vmap_area, rb_node);
			if (va->va_start <= cache->va_start) {
				free_vmap_cache = rb_prev(&va->rb_node);
				/*
				 * We don't try to update cached_hole_size or
				 * cached_align, but it won't go very wrong.
				 */
			}
		}
	}
	rb_erase(&va->rb_node, &vmap_area_root);
	RB_CLEAR_NODE(&va->rb_node);
	list_del_rcu(&va->list);

	/*
	 * Track the highest possible candidate for pcpu area
	 * allocation.  Areas outside of vmalloc area can be returned
	 * here too, consider only end addresses which fall inside
	 * vmalloc area proper.
	 */
	if (va->va_end > VMALLOC_START && va->va_end <= VMALLOC_END)
		vmap_area_pcpu_hole = max(vmap_area_pcpu_hole, va->va_end);

	kfree_rcu(va, rcu_head);
}

/*
 * Free a region of KVA allocated by alloc_vmap_area
 */
static void free_vmap_area(struct vmap_area *va)
{
	spin_lock(&vmap_area_lock);
	__free_vmap_area(va);
	spin_unlock(&vmap_area_lock);
}

/*
 * Clear the pagetable entries of a given vmap_area
 */
// ARM10C 20160827
// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
static void unmap_vmap_area(struct vmap_area *va)
{
	// va->va_start: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
	// va->va_end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	vunmap_page_range(va->va_start, va->va_end);

	// vunmap_page_range 에서 한일:
	// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함
}

// ARM10C 20160827
// va->va_start: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
// va->va_end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
static void vmap_debug_free_range(unsigned long start, unsigned long end)
{
	/*
	 * Unmap page tables and force a TLB flush immediately if
	 * CONFIG_DEBUG_PAGEALLOC is set. This catches use after free
	 * bugs similarly to those in linear kernel virtual address
	 * space after a page has been freed.
	 *
	 * All the lazy freeing logic is still retained, in order to
	 * minimise intrusiveness of this debugging feature.
	 *
	 * This is going to be *slow* (linear kernel virtual address
	 * debugging doesn't do a broadcast TLB flush so it is a lot
	 * faster).
	 */
#ifdef CONFIG_DEBUG_PAGEALLOC // CONFIG_DEBUG_PAGEALLOC=n
	vunmap_page_range(start, end);
	flush_tlb_kernel_range(start, end);
#endif
}

/*
 * lazy_max_pages is the maximum amount of virtual address space we gather up
 * before attempting to purge with a TLB flush.
 *
 * There is a tradeoff here: a larger number will cover more kernel page tables
 * and take slightly longer to purge, but it will linearly reduce the number of
 * global TLB flushes that must be performed. It would seem natural to scale
 * this number up linearly with the number of CPUs (because vmapping activity
 * could also scale linearly with the number of CPUs), however it is likely
 * that in practice, workloads might be constrained in other ways that mean
 * vmap activity will not scale linearly with CPUs. Also, I want to be
 * conservative and not introduce a big latency on huge systems, so go with
 * a less aggressive log scale. It will still be an improvement over the old
 * code, and it will be simple to change the scale factor if we find that it
 * becomes a problem on bigger systems.
 */
// ARM10C 20160827
static unsigned long lazy_max_pages(void)
{
	unsigned int log;

	// num_online_cpus(): 1, fls(1): 1
	log = fls(num_online_cpus());
	// log: 1

	// log: 1, PAGE_SIZE: 0x1000
	return log * (32UL * 1024 * 1024 / PAGE_SIZE);
	// return 0x2000 (8K)
}

// ARM10C 20160827
// ATOMIC_INIT(0): { (0) }
static atomic_t vmap_lazy_nr = ATOMIC_INIT(0);

/* for per-CPU blocks */
static void purge_fragmented_blocks_allcpus(void);

/*
 * called before a call to iounmap() if the caller wants vm_area_struct's
 * immediately freed.
 */
void set_iounmap_nonlazy(void)
{
	atomic_set(&vmap_lazy_nr, lazy_max_pages()+1);
}

/*
 * Purges all lazily-freed vmap areas.
 *
 * If sync is 0 then don't purge if there is already a purge in progress.
 * If force_flush is 1, then flush kernel TLBs between *start and *end even
 * if we found no lazy vmap areas to unmap (callers can use this to optimise
 * their own TLB flushing).
 * Returns with *start = min(*start, lowest purged address)
 *              *end = max(*end, highest purged address)
 */
static void __purge_vmap_area_lazy(unsigned long *start, unsigned long *end,
					int sync, int force_flush)
{
	static DEFINE_SPINLOCK(purge_lock);
	LIST_HEAD(valist);
	struct vmap_area *va;
	struct vmap_area *n_va;
	int nr = 0;

	/*
	 * If sync is 0 but force_flush is 1, we'll go sync anyway but callers
	 * should not expect such behaviour. This just simplifies locking for
	 * the case that isn't actually used at the moment anyway.
	 */
	if (!sync && !force_flush) {
		if (!spin_trylock(&purge_lock))
			return;
	} else
		spin_lock(&purge_lock);

	if (sync)
		purge_fragmented_blocks_allcpus();

	rcu_read_lock();
	list_for_each_entry_rcu(va, &vmap_area_list, list) {
		if (va->flags & VM_LAZY_FREE) {
			if (va->va_start < *start)
				*start = va->va_start;
			if (va->va_end > *end)
				*end = va->va_end;
			nr += (va->va_end - va->va_start) >> PAGE_SHIFT;
			list_add_tail(&va->purge_list, &valist);
			va->flags |= VM_LAZY_FREEING;
			va->flags &= ~VM_LAZY_FREE;
		}
	}
	rcu_read_unlock();

	if (nr)
		atomic_sub(nr, &vmap_lazy_nr);

	if (nr || force_flush)
		flush_tlb_kernel_range(*start, *end);

	if (nr) {
		spin_lock(&vmap_area_lock);
		list_for_each_entry_safe(va, n_va, &valist, purge_list)
			__free_vmap_area(va);
		spin_unlock(&vmap_area_lock);
	}
	spin_unlock(&purge_lock);
}

/*
 * Kick off a purge of the outstanding lazy areas. Don't bother if somebody
 * is already purging.
 */
static void try_purge_vmap_area_lazy(void)
{
	unsigned long start = ULONG_MAX, end = 0;

	__purge_vmap_area_lazy(&start, &end, 0, 0);
}

/*
 * Kick off a purge of the outstanding lazy areas.
 */
static void purge_vmap_area_lazy(void)
{
	unsigned long start = ULONG_MAX, end = 0;

	__purge_vmap_area_lazy(&start, &end, 1, 0);
}

/*
 * Free a vmap area, caller ensuring that the area has been unmapped
 * and flush_cache_vunmap had been called for the correct range
 * previously.
 */
// ARM10C 20160827
// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
static void free_vmap_area_noflush(struct vmap_area *va)
{
	// va->flags: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->flags, VM_LAZY_FREE: 0x01
	va->flags |= VM_LAZY_FREE;
	// va->flags: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->flags: VM_LAZY_FREE: 0x01 이 mask 된 값

	// va->va_end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end,
	// va->va_start: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start, PAGE_SHIFT: 12
	atomic_add((va->va_end - va->va_start) >> PAGE_SHIFT, &vmap_lazy_nr);

	// atomic_add 에서 한일:
	// free 되는 page 수를 계산하여 vmap_lazy_nr 에 더함

	// lazy_max_pages(): 0x2000
	if (unlikely(atomic_read(&vmap_lazy_nr) > lazy_max_pages()))
		try_purge_vmap_area_lazy();
}

/*
 * Free and unmap a vmap area, caller ensuring flush_cache_vunmap had been
 * called for the correct range previously.
 */
// ARM10C 20160827
// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
static void free_unmap_vmap_area_noflush(struct vmap_area *va)
{
	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
	unmap_vmap_area(va);

	// unmap_vmap_area 에서 한일:
	// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함

	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
	free_vmap_area_noflush(va);

	// free_vmap_area_noflush 에서 한일:
	// free 되는 page 수를 계산하여 vmap_lazy_nr 에 더함
	// vmap_lazy_nr 이 0x2000 개가 넘을 경우 purge를 수행
}

/*
 * Free and unmap a vmap area
 */
// ARM10C 20160827
// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
static void free_unmap_vmap_area(struct vmap_area *va)
{
	// va->va_start: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
	// va->va_end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
	flush_cache_vunmap(va->va_start, va->va_end);

	// flush_cache_vunmap 에서 한일:
	// cache 에 있는 변화된 값을 실제 메모리에 전부 반영

	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
	free_unmap_vmap_area_noflush(va);

	// free_unmap_vmap_area_noflush 에서 한일:
	// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함
	// free 되는 page 수를 계산하여 vmap_lazy_nr 에 더함
	// vmap_lazy_nr 이 0x2000 개가 넘을 경우 purge를 수행
}

// ARM10C 20160820
// addr: 할당받은 page의 mmu에 반영된 가상주소
static struct vmap_area *find_vmap_area(unsigned long addr)
{
	struct vmap_area *va;

	spin_lock(&vmap_area_lock);

	// spin_lock 에서 한일:
	// &vmap_area_lock 을 이용한 spin lock 수행

	// addr: 할당받은 page의 mmu에 반영된 가상주소
	// __find_vmap_area(할당받은 page의 mmu에 반영된 가상주소): 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
	va = __find_vmap_area(addr);
	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소

	// __find_vmap_area 에서 한일:
	// vmap_area_root.rb_node 에서 가지고 있는 rb tree의 주소를 기준으로
	// 할당받은 page의 mmu에 반영된 가상주소의 vmap_area 의 위치를 찾음

	spin_unlock(&vmap_area_lock);

	// spin_unlock 에서 한일:
	// &vmap_area_lock 을 이용한 spin unlock 수행

	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
	return va;
	// return 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
}

static void free_unmap_vmap_area_addr(unsigned long addr)
{
	struct vmap_area *va;

	va = find_vmap_area(addr);
	BUG_ON(!va);
	free_unmap_vmap_area(va);
}


/*** Per cpu kva allocator ***/

/*
 * vmap space is limited especially on 32 bit architectures. Ensure there is
 * room for at least 16 percpu vmap blocks per CPU.
 */
/*
 * If we had a constant VMALLOC_START and VMALLOC_END, we'd like to be able
 * to #define VMALLOC_SPACE		(VMALLOC_END-VMALLOC_START). Guess
 * instead (we just need a rough idea)
 */
#if BITS_PER_LONG == 32
#define VMALLOC_SPACE		(128UL*1024*1024)
#else
#define VMALLOC_SPACE		(128UL*1024*1024*1024)
#endif

#define VMALLOC_PAGES		(VMALLOC_SPACE / PAGE_SIZE)
#define VMAP_MAX_ALLOC		BITS_PER_LONG	/* 256K with 4K pages */
#define VMAP_BBMAP_BITS_MAX	1024	/* 4MB with 4K pages */
#define VMAP_BBMAP_BITS_MIN	(VMAP_MAX_ALLOC*2)
#define VMAP_MIN(x, y)		((x) < (y) ? (x) : (y)) /* can't use min() */
#define VMAP_MAX(x, y)		((x) > (y) ? (x) : (y)) /* can't use max() */
#define VMAP_BBMAP_BITS		\
		VMAP_MIN(VMAP_BBMAP_BITS_MAX,	\
		VMAP_MAX(VMAP_BBMAP_BITS_MIN,	\
			VMALLOC_PAGES / roundup_pow_of_two(NR_CPUS) / 16))

#define VMAP_BLOCK_SIZE		(VMAP_BBMAP_BITS * PAGE_SIZE)

// ARM10C 20131116
// ARM10C 20140809
static bool vmap_initialized __read_mostly = false;

// ARM10C 20140726
struct vmap_block_queue {
	spinlock_t lock;
	struct list_head free;
};

struct vmap_block {
	spinlock_t lock;
	struct vmap_area *va;
	unsigned long free, dirty;
	DECLARE_BITMAP(dirty_map, VMAP_BBMAP_BITS);
	struct list_head free_list;
	struct rcu_head rcu_head;
	struct list_head purge;
};

/* Queue of free and dirty vmap blocks, for allocation and flushing purposes */
// ARM10C 20140726
static DEFINE_PER_CPU(struct vmap_block_queue, vmap_block_queue);

/*
 * Radix tree of vmap blocks, indexed by address, to quickly find a vmap block
 * in the free path. Could get rid of this if we change the API to return a
 * "cookie" from alloc, to be passed to free. But no big deal yet.
 */
static DEFINE_SPINLOCK(vmap_block_tree_lock);
static RADIX_TREE(vmap_block_tree, GFP_ATOMIC);

/*
 * We should probably have a fallback mechanism to allocate virtual memory
 * out of partially filled vmap blocks. However vmap block sizing should be
 * fairly reasonable according to the vmalloc size, so it shouldn't be a
 * big problem.
 */

static unsigned long addr_to_vb_idx(unsigned long addr)
{
	addr -= VMALLOC_START & ~(VMAP_BLOCK_SIZE-1);
	addr /= VMAP_BLOCK_SIZE;
	return addr;
}

static struct vmap_block *new_vmap_block(gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	struct vmap_area *va;
	unsigned long vb_idx;
	int node, err;

	node = numa_node_id();

	vb = kmalloc_node(sizeof(struct vmap_block),
			gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!vb))
		return ERR_PTR(-ENOMEM);

	va = alloc_vmap_area(VMAP_BLOCK_SIZE, VMAP_BLOCK_SIZE,
					VMALLOC_START, VMALLOC_END,
					node, gfp_mask);
	if (IS_ERR(va)) {
		kfree(vb);
		return ERR_CAST(va);
	}

	err = radix_tree_preload(gfp_mask);
	if (unlikely(err)) {
		kfree(vb);
		free_vmap_area(va);
		return ERR_PTR(err);
	}

	spin_lock_init(&vb->lock);
	vb->va = va;
	vb->free = VMAP_BBMAP_BITS;
	vb->dirty = 0;
	bitmap_zero(vb->dirty_map, VMAP_BBMAP_BITS);
	INIT_LIST_HEAD(&vb->free_list);

	vb_idx = addr_to_vb_idx(va->va_start);
	spin_lock(&vmap_block_tree_lock);
	err = radix_tree_insert(&vmap_block_tree, vb_idx, vb);
	spin_unlock(&vmap_block_tree_lock);
	BUG_ON(err);
	radix_tree_preload_end();

	vbq = &get_cpu_var(vmap_block_queue);
	spin_lock(&vbq->lock);
	list_add_rcu(&vb->free_list, &vbq->free);
	spin_unlock(&vbq->lock);
	put_cpu_var(vmap_block_queue);

	return vb;
}

static void free_vmap_block(struct vmap_block *vb)
{
	struct vmap_block *tmp;
	unsigned long vb_idx;

	vb_idx = addr_to_vb_idx(vb->va->va_start);
	spin_lock(&vmap_block_tree_lock);
	tmp = radix_tree_delete(&vmap_block_tree, vb_idx);
	spin_unlock(&vmap_block_tree_lock);
	BUG_ON(tmp != vb);

	free_vmap_area_noflush(vb->va);
	kfree_rcu(vb, rcu_head);
}

static void purge_fragmented_blocks(int cpu)
{
	LIST_HEAD(purge);
	struct vmap_block *vb;
	struct vmap_block *n_vb;
	struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);

	rcu_read_lock();
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {

		if (!(vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS))
			continue;

		spin_lock(&vb->lock);
		if (vb->free + vb->dirty == VMAP_BBMAP_BITS && vb->dirty != VMAP_BBMAP_BITS) {
			vb->free = 0; /* prevent further allocs after releasing lock */
			vb->dirty = VMAP_BBMAP_BITS; /* prevent purging it again */
			bitmap_fill(vb->dirty_map, VMAP_BBMAP_BITS);
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
			spin_unlock(&vb->lock);
			list_add_tail(&vb->purge, &purge);
		} else
			spin_unlock(&vb->lock);
	}
	rcu_read_unlock();

	list_for_each_entry_safe(vb, n_vb, &purge, purge) {
		list_del(&vb->purge);
		free_vmap_block(vb);
	}
}

static void purge_fragmented_blocks_allcpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		purge_fragmented_blocks(cpu);
}

static void *vb_alloc(unsigned long size, gfp_t gfp_mask)
{
	struct vmap_block_queue *vbq;
	struct vmap_block *vb;
	unsigned long addr = 0;
	unsigned int order;

	BUG_ON(size & ~PAGE_MASK);
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);
	if (WARN_ON(size == 0)) {
		/*
		 * Allocating 0 bytes isn't what caller wants since
		 * get_order(0) returns funny result. Just warn and terminate
		 * early.
		 */
		return NULL;
	}
	order = get_order(size);

again:
	rcu_read_lock();
	vbq = &get_cpu_var(vmap_block_queue);
	list_for_each_entry_rcu(vb, &vbq->free, free_list) {
		int i;

		spin_lock(&vb->lock);
		if (vb->free < 1UL << order)
			goto next;

		i = VMAP_BBMAP_BITS - vb->free;
		addr = vb->va->va_start + (i << PAGE_SHIFT);
		BUG_ON(addr_to_vb_idx(addr) !=
				addr_to_vb_idx(vb->va->va_start));
		vb->free -= 1UL << order;
		if (vb->free == 0) {
			spin_lock(&vbq->lock);
			list_del_rcu(&vb->free_list);
			spin_unlock(&vbq->lock);
		}
		spin_unlock(&vb->lock);
		break;
next:
		spin_unlock(&vb->lock);
	}

	put_cpu_var(vmap_block_queue);
	rcu_read_unlock();

	if (!addr) {
		vb = new_vmap_block(gfp_mask);
		if (IS_ERR(vb))
			return vb;
		goto again;
	}

	return (void *)addr;
}

static void vb_free(const void *addr, unsigned long size)
{
	unsigned long offset;
	unsigned long vb_idx;
	unsigned int order;
	struct vmap_block *vb;

	BUG_ON(size & ~PAGE_MASK);
	BUG_ON(size > PAGE_SIZE*VMAP_MAX_ALLOC);

	flush_cache_vunmap((unsigned long)addr, (unsigned long)addr + size);

	order = get_order(size);

	offset = (unsigned long)addr & (VMAP_BLOCK_SIZE - 1);

	vb_idx = addr_to_vb_idx((unsigned long)addr);
	rcu_read_lock();
	vb = radix_tree_lookup(&vmap_block_tree, vb_idx);
	rcu_read_unlock();
	BUG_ON(!vb);

	vunmap_page_range((unsigned long)addr, (unsigned long)addr + size);

	spin_lock(&vb->lock);
	BUG_ON(bitmap_allocate_region(vb->dirty_map, offset >> PAGE_SHIFT, order));

	vb->dirty += 1UL << order;
	if (vb->dirty == VMAP_BBMAP_BITS) {
		BUG_ON(vb->free);
		spin_unlock(&vb->lock);
		free_vmap_block(vb);
	} else
		spin_unlock(&vb->lock);
}

/**
 * vm_unmap_aliases - unmap outstanding lazy aliases in the vmap layer
 *
 * The vmap/vmalloc layer lazily flushes kernel virtual mappings primarily
 * to amortize TLB flushing overheads. What this means is that any page you
 * have now, may, in a former life, have been mapped into kernel virtual
 * address by the vmap layer and so there might be some CPUs with TLB entries
 * still referencing that page (additional to the regular 1:1 kernel mapping).
 *
 * vm_unmap_aliases flushes all such lazy mappings. After it returns, we can
 * be sure that none of the pages we have control over will have any aliases
 * from the vmap layer.
 */
void vm_unmap_aliases(void)
{
	unsigned long start = ULONG_MAX, end = 0;
	int cpu;
	int flush = 0;

	if (unlikely(!vmap_initialized))
		return;

	for_each_possible_cpu(cpu) {
		struct vmap_block_queue *vbq = &per_cpu(vmap_block_queue, cpu);
		struct vmap_block *vb;

		rcu_read_lock();
		list_for_each_entry_rcu(vb, &vbq->free, free_list) {
			int i, j;

			spin_lock(&vb->lock);
			i = find_first_bit(vb->dirty_map, VMAP_BBMAP_BITS);
			if (i < VMAP_BBMAP_BITS) {
				unsigned long s, e;

				j = find_last_bit(vb->dirty_map,
							VMAP_BBMAP_BITS);
				j = j + 1; /* need exclusive index */

				s = vb->va->va_start + (i << PAGE_SHIFT);
				e = vb->va->va_start + (j << PAGE_SHIFT);
				flush = 1;

				if (s < start)
					start = s;
				if (e > end)
					end = e;
			}
			spin_unlock(&vb->lock);
		}
		rcu_read_unlock();
	}

	__purge_vmap_area_lazy(&start, &end, 1, flush);
}
EXPORT_SYMBOL_GPL(vm_unmap_aliases);

/**
 * vm_unmap_ram - unmap linear kernel address space set up by vm_map_ram
 * @mem: the pointer returned by vm_map_ram
 * @count: the count passed to that vm_map_ram call (cannot unmap partial)
 */
void vm_unmap_ram(const void *mem, unsigned int count)
{
	unsigned long size = count << PAGE_SHIFT;
	unsigned long addr = (unsigned long)mem;

	BUG_ON(!addr);
	BUG_ON(addr < VMALLOC_START);
	BUG_ON(addr > VMALLOC_END);
	BUG_ON(addr & (PAGE_SIZE-1));

	debug_check_no_locks_freed(mem, size);
	vmap_debug_free_range(addr, addr+size);

	if (likely(count <= VMAP_MAX_ALLOC))
		vb_free(mem, size);
	else
		free_unmap_vmap_area_addr(addr);
}
EXPORT_SYMBOL(vm_unmap_ram);

/**
 * vm_map_ram - map pages linearly into kernel virtual address (vmalloc space)
 * @pages: an array of pointers to the pages to be mapped
 * @count: number of pages
 * @node: prefer to allocate data structures on this node
 * @prot: memory protection to use. PAGE_KERNEL for regular RAM
 *
 * Returns: a pointer to the address that has been mapped, or %NULL on failure
 */
void *vm_map_ram(struct page **pages, unsigned int count, int node, pgprot_t prot)
{
	unsigned long size = count << PAGE_SHIFT;
	unsigned long addr;
	void *mem;

	if (likely(count <= VMAP_MAX_ALLOC)) {
		mem = vb_alloc(size, GFP_KERNEL);
		if (IS_ERR(mem))
			return NULL;
		addr = (unsigned long)mem;
	} else {
		struct vmap_area *va;
		va = alloc_vmap_area(size, PAGE_SIZE,
				VMALLOC_START, VMALLOC_END, node, GFP_KERNEL);
		if (IS_ERR(va))
			return NULL;

		addr = va->va_start;
		mem = (void *)addr;
	}
	if (vmap_page_range(addr, addr + size, prot, pages) < 0) {
		vm_unmap_ram(mem, count);
		return NULL;
	}
	return mem;
}
EXPORT_SYMBOL(vm_map_ram);

// ARM10C 20131116
// ARM10C 20131130
// ARM10C 20140809
static struct vm_struct *vmlist __initdata;
/**
 * vm_area_add_early - add vmap area early during boot
 * @vm: vm_struct to add
 *
 * This function is used to add fixed kernel vm area to vmlist before
 * vmalloc_init() is called.  @vm->addr, @vm->size, and @vm->flags
 * should contain proper values and the other fields should be zero.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
// ARM10C 20131116
// ARM10C 20131130
// vm->addr: 0xF8000000
// vm->size: 0x1000
// vm->phys_addr: 0x10000000
// vm->flags: 0x40000001
//
// S3C_VA_SYS
// vm->addr: 0xF6100000
// vm->size: 0x10000 
// vm->phys_addr: 0x10050000
// vm->flags: 0x40000001
void __init vm_area_add_early(struct vm_struct *vm)
{
	struct vm_struct *tmp, **p;

	// vmap_initialized: false
	BUG_ON(vmap_initialized);
	for (p = &vmlist; (tmp = *p) != NULL; p = &tmp->next) {
		if (tmp->addr >= vm->addr) {
			// 이전의 vm영역을 침범하는지 확인
			BUG_ON(tmp->addr < vm->addr + vm->size);
			break;
		} else
			BUG_ON(tmp->addr + tmp->size > vm->addr);
	}
	vm->next = *p;
	*p = vm;
}

/**
 * vm_area_register_early - register vmap area early during boot
 * @vm: vm_struct to register
 * @align: requested alignment
 *
 * This function is used to register kernel vm area before
 * vmalloc_init() is called.  @vm->size and @vm->flags should contain
 * proper values on entry and other fields should be zero.  On return,
 * vm->addr contains the allocated address.
 *
 * DO NOT USE THIS FUNCTION UNLESS YOU KNOW WHAT YOU'RE DOING.
 */
void __init vm_area_register_early(struct vm_struct *vm, size_t align)
{
	static size_t vm_init_off __initdata;
	unsigned long addr;

	addr = ALIGN(VMALLOC_START + vm_init_off, align);
	vm_init_off = PFN_ALIGN(addr + vm->size) - VMALLOC_START;

	vm->addr = (void *)addr;

	vm_area_add_early(vm);
}

// ARM10C 20140726
void __init vmalloc_init(void)
{
	struct vmap_area *va;
	struct vm_struct *tmp;
	int i;

	for_each_possible_cpu(i) {
	// for ((i) = -1; (i) = cpumask_next((i), (cpu_possible_mask)), (i) < nr_cpu_ids; )

		struct vmap_block_queue *vbq;
		struct vfree_deferred *p;

		// i: 0, per_cpu(vmap_block_queue, 0): *(&vmap_block_queue + __per_cpu_offset[0])
		vbq = &per_cpu(vmap_block_queue, i);
		// vbq: &vmap_block_queue + __per_cpu_offset[0]

		// &vbq->lock: &(&vmap_block_queue + __per_cpu_offset[0])->lock
		spin_lock_init(&vbq->lock);
		// &(&vmap_block_queue + __per_cpu_offset[0])->lock 을 이용한 spinlock 초기화

		// &vbq->free: &(&vmap_block_queue + __per_cpu_offset[0])->free
		INIT_LIST_HEAD(&vbq->free);
		// &vbq->free: &(&vmap_block_queue + __per_cpu_offset[0])->free 리스트 초기화

		// i: 0, per_cpu(vfree_deferred, 0): *(&vfree_deferred + __per_cpu_offset[0])
		p = &per_cpu(vfree_deferred, i);
		// p: &vfree_deferred + __per_cpu_offset[0]

		// p->list: (&vfree_deferred + __per_cpu_offset[0])->list
		init_llist_head(&p->list);
		// llist의 first를 NULL로 초기화

		// p->wq: (&vfree_deferred + __per_cpu_offset[0])->wq
		INIT_WORK(&p->wq, free_work);
		// wq의 member를 초기화

		// [loop 2 .. 3] 수행은 skip
	}

	/* Import existing vmlist entries. */

	// ioremap.c 에서 static_vmlist 로 이전에 추가해 놓은 vm 정보들
	// SYSC: 0xf6100000 +  64kB   PA:0x10050000
	// TMR : 0xf6300000 +  16kB   PA:0x12DD0000
	// WDT : 0xf6400000 +   4kB   PA:0x101D0000
	// CHID: 0xf8000000 +   4kB   PA:0x10000000
	// CMU : 0xf8100000 + 144kB   PA:0x10010000
	// PMU : 0xf8180000 +  64kB   PA:0x10040000
	// SRAM: 0xf8400000 +   4kB   PA:0x02020000
	// ROMC: 0xf84c0000 +   4kB   PA:0x12250000
	//
	// SYSC:
	// va_start: 0xf6100000, va_end: 0xf6110000
	// TMR:
	// va_start: 0xf6300000, va_end: 0xf6304000
	// WDT:
	// va_start: 0xf6400000, va_end: 0xf6401000
	// CHID:
	// va_start: 0xf8000000, va_end: 0xf8001000
	// CMU:
	// va_start: 0xf8100000, va_end: 0xf8124000
	// PMU:
	// va_start: 0xf8180000, va_end: 0xf8190000
	// SRAM:
	// va_start: 0xf8400000, va_end: 0xf8401000
	// ROMC:
	// va_start: 0xf84c0000, va_end: 0xf84c1000
	for (tmp = vmlist; tmp; tmp = tmp->next) {
		// tmp: SYSC

		// sizeof(struct vmap_area): 52 bytes, GFP_NOWAIT: 0
		// kzalloc(52, 0): kmem_cache#30-o9
		va = kzalloc(sizeof(struct vmap_area), GFP_NOWAIT);
		// va: kmem_cache#30-o9

		// va->flags: (kmem_cache#30-o9)->flags, VM_VM_AREA: 0x04
		va->flags = VM_VM_AREA;
		// va->flags: (kmem_cache#30-o9)->flags: 0x04

		// va->va_start: (kmem_cache#30-o9)->va_start, tmp->addr: 0xf6100000
		va->va_start = (unsigned long)tmp->addr;
		// va->va_start: (kmem_cache#30-o9)->va_start: 0xf6100000

		// va->va_end: (kmem_cache#30-o9)->va_end
		// va->va_start: (kmem_cache#30-o9)->va_start: 0xf6100000, tmp->size: 0x10000
		va->va_end = va->va_start + tmp->size;
		// va->va_end: (kmem_cache#30-o9)->va_end: 0xf6110000

		// va->vm: (kmem_cache#30-o9)->vm, tmp: SYSC
		va->vm = tmp;
		// va->vm: (kmem_cache#30-o9)->vm: SYSC

		// va: kmem_cache#30-o9
		__insert_vmap_area(va);
		// vm SYSC 정보를 RB Tree 구조로 삽입

		// tmp가 TMR WDT CHID CMU PMU SRAM ROMC
		// 순서로 루프 수행
	}

	// VMALLOC_END: 0xff000000UL
	vmap_area_pcpu_hole = VMALLOC_END;
	// vmap_area_pcpu_hole: 0xff000000UL

	// vmap_initialized: false
	vmap_initialized = true;
	// vmap_initialized: true
}

/**
 * map_kernel_range_noflush - map kernel VM area with the specified pages
 * @addr: start of the VM area to map
 * @size: size of the VM area to map
 * @prot: page protection flags to use
 * @pages: pages to map
 *
 * Map PFN_UP(@size) pages at @addr.  The VM area @addr and @size
 * specify should have been allocated using get_vm_area() and its
 * friends.
 *
 * NOTE:
 * This function does NOT do any cache flushing.  The caller is
 * responsible for calling flush_cache_vmap() on to-be-mapped areas
 * before calling this function.
 *
 * RETURNS:
 * The number of pages mapped on success, -errno on failure.
 */
int map_kernel_range_noflush(unsigned long addr, unsigned long size,
			     pgprot_t prot, struct page **pages)
{
	return vmap_page_range_noflush(addr, addr + size, prot, pages);
}

/**
 * unmap_kernel_range_noflush - unmap kernel VM area
 * @addr: start of the VM area to unmap
 * @size: size of the VM area to unmap
 *
 * Unmap PFN_UP(@size) pages at @addr.  The VM area @addr and @size
 * specify should have been allocated using get_vm_area() and its
 * friends.
 *
 * NOTE:
 * This function does NOT do any cache flushing.  The caller is
 * responsible for calling flush_cache_vunmap() on to-be-mapped areas
 * before calling this function and flush_tlb_kernel_range() after.
 */
void unmap_kernel_range_noflush(unsigned long addr, unsigned long size)
{
	vunmap_page_range(addr, addr + size);
}
EXPORT_SYMBOL_GPL(unmap_kernel_range_noflush);

/**
 * unmap_kernel_range - unmap kernel VM area and flush cache and TLB
 * @addr: start of the VM area to unmap
 * @size: size of the VM area to unmap
 *
 * Similar to unmap_kernel_range_noflush() but flushes vcache before
 * the unmapping and tlb after.
 */
void unmap_kernel_range(unsigned long addr, unsigned long size)
{
	unsigned long end = addr + size;

	flush_cache_vunmap(addr, end);
	vunmap_page_range(addr, end);
	flush_tlb_kernel_range(addr, end);
}

// ARM10C 20160813
// area: kmem_cache#30-oX (vm_struct), prot: pgprot_kernel에 0x204 를 or 한 값, pages: &&(page 1개(4K)의 할당된 메모리 주소)
int map_vm_area(struct vm_struct *area, pgprot_t prot, struct page ***pages)
{
	// area->addr: (kmem_cache#30-oX (vm_struct))->addr: 할당 받은 가상 주소값
	unsigned long addr = (unsigned long)area->addr;
	// addr: 할당 받은 가상 주소값

	// addr: 할당 받은 가상 주소값, area: kmem_cache#30-oX (vm_struct)
	// get_vm_area_size(kmem_cache#30-oX (vm_struct)): 0x1000
	unsigned long end = addr + get_vm_area_size(area);
	// end: 할당 받은 가상 주소값+ 0x1000

	int err;

	// addr: 할당 받은 가상 주소값, end: 할당 받은 가상 주소값+ 0x1000,
	// prot: pgprot_kernel에 0x204 를 or 한 값, *pages: &(page 1개(4K)의 할당된 메모리 주소)
	// vmap_page_range(addr: 할당 받은 가상 주소값, 할당 받은 가상 주소값+ 0x1000,
	// pgprot_kernel에 0x204 를 or 한 값, &(page 1개(4K)의 할당된 메모리 주소)): 1
	err = vmap_page_range(addr, end, prot, *pages);
	// err: 1

	// vmap_page_range 에서 한일:
	// 할당 받은 가상 주소값을 가지고 있는 page table section 하위 pte table을 갱신함
	// cache의 값을 전부 메모리에 반영

	// err: 1
	if (err > 0) {
		// *pages: &(page 1개(4K)의 할당된 메모리 주소), err: 1
		*pages += err;
		// *pages: &(page 1개(4K)의 할당된 메모리 주소) + 1

		// err: 1
		err = 0;
		// err: 0
	}

	// err: 0
	return err;
	// return 0
}
EXPORT_SYMBOL_GPL(map_vm_area);

// ARM10C 20141025
// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area GIC#0), flags: GFP_KERNEL: 0xD0
// caller: __builtin_return_address(0)
// ARM10C 20141108
// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area GIC#1), flags: GFP_KERNEL: 0xD0
// caller: __builtin_return_address(0)
// ARM10C 20141206
// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area COMB), flags: GFP_KERNEL: 0xD0
// caller: __builtin_return_address(0)
// ARM10C 20150110
// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area CLK), flags: GFP_KERNEL: 0xD0
// caller: __builtin_return_address(0)
// ARM10C 20150321
// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area MCT), flags: GFP_KERNEL: 0xD0
// caller: __builtin_return_address(0)
static void setup_vmalloc_vm(struct vm_struct *vm, struct vmap_area *va,
			      unsigned long flags, const void *caller)
{
	spin_lock(&vmap_area_lock);
	// vmap_area_lock을 이용한 spinlock 설정 수행
	// vmap_area_lock을 이용한 spinlock 설정 수행

	// vm->flags: (kmem_cache#30-oX (vm_struct))->flags
	// vm->flags: (kmem_cache#30-oX (vm_struct))->flags
	vm->flags = flags;
	// vm->flags: (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0
	// vm->flags: (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0

	// vm->addr: (kmem_cache#30-oX (vm_struct))->addr, va->va_start: (kmem_cache#30-oX (vmap_area GIC#0))->va_start: 0xf0000000
	// vm->addr: (kmem_cache#30-oX (vm_struct))->addr, va->va_start: (kmem_cache#30-oX (vmap_area GIC#1))->va_start: 0xf0002000
	vm->addr = (void *)va->va_start;
	// vm->addr: (kmem_cache#30-oX (vm_struct))->addr: 0xf0000000
	// vm->addr: (kmem_cache#30-oX (vm_struct))->addr: 0xf0002000

	// vm->size: (kmem_cache#30-oX (vm_struct))->size,
	// va->va_start: (kmem_cache#30-oX (vmap_area GIC#0))->va_start: 0xf0000000,
	// va->va_end: (kmem_cache#30-oX (vmap_area GIC#0))->va_end: 0xf0002000
	// vm->size: (kmem_cache#30-oX (vm_struct))->size,
	// va->va_start: (kmem_cache#30-oX (vmap_area GIC#1))->va_start: 0xf0002000,
	// va->va_end: (kmem_cache#30-oX (vmap_area GIC#1))->va_end: 0xf0004000
	vm->size = va->va_end - va->va_start;
	// vm->size: (kmem_cache#30-oX (vm_struct))->size: 0x2000
	// vm->size: (kmem_cache#30-oX (vm_struct))->size: 0x2000

	// vm->caller: (kmem_cache#30-oX (vm_struct))->caller, caller: __builtin_return_address(0)
	// vm->caller: (kmem_cache#30-oX (vm_struct))->caller, caller: __builtin_return_address(0)
	vm->caller = caller;
	// vm->caller: (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)
	// vm->caller: (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)

	// va->vm: (kmem_cache#30-oX (vmap_area GIC#0))->vm, vm: kmem_cache#30-oX (vm_struct)
	// va->vm: (kmem_cache#30-oX (vmap_area GIC#1))->vm, vm: kmem_cache#30-oX (vm_struct)
	va->vm = vm;
	// va->vm: (kmem_cache#30-oX (vmap_area GIC#0))->vm: kmem_cache#30-oX (vm_struct)
	// va->vm: (kmem_cache#30-oX (vmap_area GIC#1))->vm: kmem_cache#30-oX (vm_struct)

	// va->flags: (kmem_cache#30-oX (vmap_area GIC#0))->flags: 0, VM_VM_AREA: 0x04
	// va->flags: (kmem_cache#30-oX (vmap_area GIC#1))->flags: 0, VM_VM_AREA: 0x04
	va->flags |= VM_VM_AREA;
	// va->flags: (kmem_cache#30-oX (vmap_area GIC#0))->flags: 0x04
	// va->flags: (kmem_cache#30-oX (vmap_area GIC#1))->flags: 0x04

	spin_unlock(&vmap_area_lock);
	// vmap_area_lock을 이용한 spinlock 해재 수행
	// vmap_area_lock을 이용한 spinlock 해재 수행
}

static void clear_vm_uninitialized_flag(struct vm_struct *vm)
{
	/*
	 * Before removing VM_UNINITIALIZED,
	 * we should make sure that vm has proper values.
	 * Pair with smp_rmb() in show_numa_info().
	 */
	smp_wmb();
	vm->flags &= ~VM_UNINITIALIZED;
}

// ARM10C 20141025
// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
// ARM10C 20141101
// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
// ARM10C 20141206
// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
// ARM10C 20150110
// size: 0x30000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
// ARM10C 20150321
// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
static struct vm_struct *__get_vm_area_node(unsigned long size,
		unsigned long align, unsigned long flags, unsigned long start,
		unsigned long end, int node, gfp_t gfp_mask, const void *caller)
{
	struct vmap_area *va;
	struct vm_struct *area;

	// in_interrupt(): 0
	// in_interrupt(): 0
	// in_interrupt(): 0
	// in_interrupt(): 0
	// in_interrupt(): 0
	BUG_ON(in_interrupt());

	// flags: VM_IOREMAP: 0x00000001
	// flags: VM_IOREMAP: 0x00000001
	// flags: VM_IOREMAP: 0x00000001
	// flags: VM_IOREMAP: 0x00000001
	// flags: VM_IOREMAP: 0x00000001
	if (flags & VM_IOREMAP)
		// size: 0x1000, fls(0x1000): 13, PAGE_SHIFT: 12, IOREMAP_MAX_ORDER: 24
		// clamp(13, 12, 24): 13
		// size: 0x1000, fls(0x1000): 13, PAGE_SHIFT: 12, IOREMAP_MAX_ORDER: 24
		// clamp(13, 12, 24): 13
		// size: 0x1000, fls(0x1000): 13, PAGE_SHIFT: 12, IOREMAP_MAX_ORDER: 24
		// clamp(13, 12, 24): 13
		// size: 0x30000, fls(0x30000): 18, PAGE_SHIFT: 12, IOREMAP_MAX_ORDER: 24
		// clamp(18, 12, 24): 18
		// size: 0x1000, fls(1000): 13, PAGE_SHIFT: 12, IOREMAP_MAX_ORDER: 24
		// clamp(13, 12, 24): 13
		align = 1ul << clamp(fls(size), PAGE_SHIFT, IOREMAP_MAX_ORDER);
		// align: 0x2000
		// align: 0x2000
		// align: 0x2000
		// align: 0x40000
		// align: 0x2000

	// size: 0x1000
	// size: 0x1000
	// size: 0x1000
	// size: 0x30000
	// size: 0x1000
	size = PAGE_ALIGN(size);
	// size: 0x1000
	// size: 0x1000
	// size: 0x1000
	// size: 0x30000
	// size: 0x1000

	// size: 0x1000
	// size: 0x1000
	// size: 0x1000
	// size: 0x30000
	// size: 0x1000
	if (unlikely(!size))
		return NULL;

	// sizeof(*area): 32, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// kzalloc_node(32, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX (vm_struct)
	// sizeof(*area): 32, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// kzalloc_node(32, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX (vm_struct)-2
	// sizeof(*area): 32, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// kzalloc_node(32, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX (vm_struct)-3
	// sizeof(*area): 32, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// kzalloc_node(32, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX (vm_struct)-4
	// sizeof(*area): 32, gfp_mask: GFP_KERNEL: 0xD0, GFP_RECLAIM_MASK: 0x13ef0, node: -1
	// kzalloc_node(32, GFP_KERNEL: 0xD0, -1): kmem_cache#30-oX (vm_struct)-5
	area = kzalloc_node(sizeof(*area), gfp_mask & GFP_RECLAIM_MASK, node);
	// area: kmem_cache#30-oX (vm_struct)
	// area: kmem_cache#30-oX (vm_struct)-2
	// area: kmem_cache#30-oX (vm_struct)-3
	// area: kmem_cache#30-oX (vm_struct)-4
	// area: kmem_cache#30-oX (vm_struct)-5

	// area: kmem_cache#30-oX (vm_struct)
	// area: kmem_cache#30-oX (vm_struct)-2
	// area: kmem_cache#30-oX (vm_struct)-3
	// area: kmem_cache#30-oX (vm_struct)-4
	// area: kmem_cache#30-oX (vm_struct)-5
	if (unlikely(!area))
		return NULL;

	/*
	 * We always allocate a guard page.
	 */
	// size: 0x1000, PAGE_SIZE: 0x1000
	// size: 0x1000, PAGE_SIZE: 0x1000
	// size: 0x1000, PAGE_SIZE: 0x1000
	// size: 0x30000, PAGE_SIZE: 0x1000
	// size: 0x1000, PAGE_SIZE: 0x1000
	size += PAGE_SIZE;
	// size: 0x2000
	// size: 0x2000
	// size: 0x2000
	// size: 0x31000
	// size: 0x2000

// 2014/11/01 종료
// 2014/11/08 시작

	// size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
	// alloc_vmap_area(0x2000, 0x2000, 0xf0000000, 0xff000000, -1, GFP_KERNEL: 0xD0): kmem_cache#30-oX (vmap_area GIC#0)
	// size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
	// alloc_vmap_area(0x2000, 0x2000, 0xf0000000, 0xff000000, -1, GFP_KERNEL: 0xD0): kmem_cache#30-oX (vmap_area GIC#1)
	// size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
	// alloc_vmap_area(0x2000, 0x2000, 0xf0000000, 0xff000000, -1, GFP_KERNEL: 0xD0): kmem_cache#30-oX (vmap_area COMB)
	// size: 0x31000, align: 0x40000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
	// alloc_vmap_area(0x31000, 0x40000, 0xf0000000, 0xff000000, -1, GFP_KERNEL: 0xD0): kmem_cache#30-oX (vmap_area CLK)
	// size: 0x2000, align: 0x2000, start: 0xf0000000, end: 0xff000000, node: -1, gfp_mask: GFP_KERNEL: 0xD0
	// alloc_vmap_area(0x2000, 0x2000, 0xf0000000, 0xff000000, -1, GFP_KERNEL: 0xD0): kmem_cache#30-oX (vmap_area MCT)
	va = alloc_vmap_area(size, align, start, end, node, gfp_mask);
	// va: kmem_cache#30-oX (vmap_area GIC#0)
	// va: kmem_cache#30-oX (vmap_area GIC#1)
	// va: kmem_cache#30-oX (vmap_area COMB)
	// va: kmem_cache#30-oX (vmap_area CLK)
	// va: kmem_cache#30-oX (vmap_area MCT)

	/*
	// alloc_vmap_area에서 한일:
	// alloc area (GIC#0) 를 만들고 rb tree에 alloc area 를 추가
	// 가상주소 va_start 기준으로 GIC#0 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                 SYSC-b      WDT-b         CMU-b         SRAM-b
	//            (0xF6100000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /                                                 \
	//        GIC#0-r                                                 ROMC-r
	//   (0xF0000000)                                                 (0xF84C0000)
	//
	// vmap_area_list에 GIC#0 - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// alloc_vmap_area에서 한일:
	// alloc area (GIC#1) 를 만들고 rb tree에 alloc area 를 추가
	// 가상주소 va_start 기준으로 GIC#1 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-r               PMU-r
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-b      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-r     SYSC-r                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// alloc_vmap_area에서 한일:
	// alloc area (COMB) 를 만들고 rb tree에 alloc area 를 추가
	// 가상주소 va_start 기준으로 COMB 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     SYSC-b                                       ROMC-r
	//    (0xF0000000)   (0xF6100000)                                 (0xF84C0000)
	//                   /
	//               COMB-r
	//          (0xF0004000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - COMB - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// alloc_vmap_area에서 한일:
	// alloc area (CLK) 를 만들고 rb tree에 alloc area 를 추가
	// 가상주소 va_start 기준으로 CLK 를 RB Tree 추가한 결과
	//
	//                                  CHID-b
	//                               (0xF8000000)
	//                              /            \
	//                         TMR-b               PMU-b
	//                    (0xF6300000)             (0xF8180000)
	//                      /      \               /           \
	//                GIC#1-r      WDT-b         CMU-b         SRAM-b
	//            (0xF0002000)   (0xF6400000)  (0xF8100000)   (0xF8400000)
	//             /       \                                          \
	//        GIC#0-b     CLK-b                                        ROMC-r
	//    (0xF0000000)   (0xF0040000)                                 (0xF84C0000)
	//                   /      \
	//               COMB-r     SYSC-r
	//          (0xF0004000)   (0xF6100000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - COMB - CLK - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	/*
	// alloc_vmap_area에서 한일:
	// alloc area (MCT) 를 만들고 rb tree에 alloc area 를 추가
	// 가상주소 va_start 기준으로 MCT 를 RB Tree 추가한 결과
	//
	//                                      CHID-b
	//                                    (0xF8000000)
	//                                  /              \
	//                            CLK-b                  PMU-b
	//                         (0xF0040000)              (0xF8180000)
	//                        /          \                /        \
	//                 GIC#1-r            TMR-r        CMU-b         SRAM-b
	//             (0xF0002000)         (0xF6300000)   (0xF8100000)  (0xF8400000)
	//              /       \              /    \                         \
	//        GIC#0-b       COMB-b     SYSC-b     WDT-b                   ROMC-r
	//    (0xF0000000) (0xF0004000) (0xF6100000)  (0xF6400000)            (0xF84C0000)
	//                          \
	//                          MCT-r
	//                       (0xF0006000)
	//
	// vmap_area_list에 GIC#0 - GIC#1 - COMB - MCT - CLK - SYSC -TMR - WDT - CHID - CMU - PMU - SRAM - ROMC
	// 순서로 리스트에 연결이 됨
	*/

	// va: kmem_cache#30-oX (vmap_area GIC#0), IS_ERR(kmem_cache#30-oX): 0
	// va: kmem_cache#30-oX (vmap_area GIC#1), IS_ERR(kmem_cache#30-oX): 0
	// va: kmem_cache#30-oX (vmap_area COMB), IS_ERR(kmem_cache#30-oX): 0
	// va: kmem_cache#30-oX (vmap_area CLK), IS_ERR(kmem_cache#30-oX): 0
	// va: kmem_cache#30-oX (vmap_area MCT), IS_ERR(kmem_cache#30-oX): 0
	if (IS_ERR(va)) {
		kfree(area);
		return NULL;
	}

	// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area GIC#0), flags: GFP_KERNEL: 0xD0
	// caller: __builtin_return_address(0)
	// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area GIC#1), flags: GFP_KERNEL: 0xD0
	// caller: __builtin_return_address(0)
	// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area COMB), flags: GFP_KERNEL: 0xD0
	// caller: __builtin_return_address(0)
	// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area CLK), flags: GFP_KERNEL: 0xD0
	// caller: __builtin_return_address(0)
	// area: kmem_cache#30-oX (vm_struct), va: kmem_cache#30-oX (vmap_area MCT), flags: GFP_KERNEL: 0xD0
	// caller: __builtin_return_address(0)
	setup_vmalloc_vm(area, va, flags, caller);

	// setup_vmalloc_vm이 한일:
	// (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0
	// (kmem_cache#30-oX (vm_struct))->addr: 0xf0000000
	// (kmem_cache#30-oX (vm_struct))->size: 0x2000
	// (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)
	//
	// (kmem_cache#30-oX (vmap_area GIC#0))->vm: kmem_cache#30-oX (vm_struct)
	// (kmem_cache#30-oX (vmap_area GIC#0))->flags: 0x04

	// setup_vmalloc_vm이 한일:
	// (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0
	// (kmem_cache#30-oX (vm_struct))->addr: 0xf0002000
	// (kmem_cache#30-oX (vm_struct))->size: 0x2000
	// (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)
	//
	// (kmem_cache#30-oX (vmap_area GIC#1))->vm: kmem_cache#30-oX (vm_struct)
	// (kmem_cache#30-oX (vmap_area GIC#1))->flags: 0x04

	// setup_vmalloc_vm이 한일:
	// (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0
	// (kmem_cache#30-oX (vm_struct))->addr: 0xf0004000
	// (kmem_cache#30-oX (vm_struct))->size: 0x2000
	// (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)
	//
	// (kmem_cache#30-oX (vmap_area COMB))->vm: kmem_cache#30-oX (vm_struct)
	// (kmem_cache#30-oX (vmap_area COMB))->flags: 0x04

	// setup_vmalloc_vm이 한일:
	// (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0
	// (kmem_cache#30-oX (vm_struct))->addr: 0xf0040000
	// (kmem_cache#30-oX (vm_struct))->size: 0x31000
	// (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)
	//
	// (kmem_cache#30-oX (vmap_area CLK))->vm: kmem_cache#30-oX (vm_struct)
	// (kmem_cache#30-oX (vmap_area CLK))->flags: 0x04

	// setup_vmalloc_vm이 한일:
	// (kmem_cache#30-oX (vm_struct))->flags: GFP_KERNEL: 0xD0
	// (kmem_cache#30-oX (vm_struct))->addr: 0xf0006000
	// (kmem_cache#30-oX (vm_struct))->size: 0x2000
	// (kmem_cache#30-oX (vm_struct))->caller: __builtin_return_address(0)
	//
	// (kmem_cache#30-oX (vmap_area CLK))->vm: kmem_cache#30-oX (vm_struct)
	// (kmem_cache#30-oX (vmap_area CLK))->flags: 0x04

	// area: kmem_cache#30-oX (vm_struct)
	// area: kmem_cache#30-oX (vm_struct)
	// area: kmem_cache#30-oX (vm_struct)
	// area: kmem_cache#30-oX (vm_struct)
	// area: kmem_cache#30-oX (vm_struct)
	return area;
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
}

struct vm_struct *__get_vm_area(unsigned long size, unsigned long flags,
				unsigned long start, unsigned long end)
{
	return __get_vm_area_node(size, 1, flags, start, end, NUMA_NO_NODE,
				  GFP_KERNEL, __builtin_return_address(0));
}
EXPORT_SYMBOL_GPL(__get_vm_area);

struct vm_struct *__get_vm_area_caller(unsigned long size, unsigned long flags,
				       unsigned long start, unsigned long end,
				       const void *caller)
{
	return __get_vm_area_node(size, 1, flags, start, end, NUMA_NO_NODE,
				  GFP_KERNEL, caller);
}

/**
 *	get_vm_area  -  reserve a contiguous kernel virtual area
 *	@size:		size of the area
 *	@flags:		%VM_IOREMAP for I/O mappings or VM_ALLOC
 *
 *	Search an area of @size in the kernel virtual mapping area,
 *	and reserved it for out purposes.  Returns the area descriptor
 *	on success or %NULL on failure.
 */
struct vm_struct *get_vm_area(unsigned long size, unsigned long flags)
{
	return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL,
				  __builtin_return_address(0));
}

// ARM10C 20141025
// size: 0x1000, VM_IOREMAP: 0x00000001, caller: __builtin_return_address(0)
// ARM10C 20141101
// size: 0x1000, VM_IOREMAP: 0x00000001, caller: __builtin_return_address(0)
// ARM10C 20141206
// size: 0x1000, VM_IOREMAP: 0x00000001, caller: __builtin_return_address(0)
// ARM10C 20150110
// size: 0x30000, VM_IOREMAP: 0x00000001, caller: __builtin_return_address(0)
// ARM10C 20150321
// size: 0x1000, VM_IOREMAP: 0x00000001, caller: __builtin_return_address(0)
// ARM10C 20160813
// 0x1000, flags: 0x00000001, __builtin_return_address(0)
struct vm_struct *get_vm_area_caller(unsigned long size, unsigned long flags,
				const void *caller)
{
	// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
	// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
	// __get_vm_area_node(0x1000, VM_IOREMAP: 0x00000001, 0xf0000000, 0xff000000UL, -1, GFP_KERNEL: 0xD0, __builtin_return_address(0)):
	// kmem_cache#30-oX (vm_struct)
	// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
	// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
	// __get_vm_area_node(0x1000, VM_IOREMAP: 0x00000001, 0xf0000000, 0xff000000UL, -1, GFP_KERNEL: 0xD0, __builtin_return_address(0)):
	// kmem_cache#30-oX (vm_struct)
	// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
	// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
	// __get_vm_area_node(0x1000, VM_IOREMAP: 0x00000001, 0xf0000000, 0xff000000UL, -1, GFP_KERNEL: 0xD0, __builtin_return_address(0)):
	// kmem_cache#30-oX (vm_struct)
	// size: 0x30000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
	// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
	// __get_vm_area_node(0x30000, VM_IOREMAP: 0x00000001, 0xf0000000, 0xff000000UL, -1, GFP_KERNEL: 0xD0, __builtin_return_address(0)):
	// kmem_cache#30-oX (vm_struct)
	// size: 0x1000, 1, VM_IOREMAP: 0x00000001, VMALLOC_START: 0xf0000000, VMALLOC_END: 0xff000000UL,
	// NUMA_NO_NODE: -1, GFP_KERNEL: 0xD0, caller: __builtin_return_address(0)
	// __get_vm_area_node(0x1000, VM_IOREMAP: 0x00000001, 0xf0000000, 0xff000000UL, -1, GFP_KERNEL: 0xD0, __builtin_return_address(0)):
	// kmem_cache#30-oX (vm_struct)
	return __get_vm_area_node(size, 1, flags, VMALLOC_START, VMALLOC_END,
				  NUMA_NO_NODE, GFP_KERNEL, caller);
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
	// return kmem_cache#30-oX (vm_struct)
}

/**
 *	find_vm_area  -  find a continuous kernel virtual area
 *	@addr:		base address
 *
 *	Search for the kernel VM area starting at @addr, and return it.
 *	It is up to the caller to do all required locking to keep the returned
 *	pointer valid.
 */
struct vm_struct *find_vm_area(const void *addr)
{
	struct vmap_area *va;

	va = find_vmap_area((unsigned long)addr);
	if (va && va->flags & VM_VM_AREA)
		return va->vm;

	return NULL;
}

/**
 *	remove_vm_area  -  find and remove a continuous kernel virtual area
 *	@addr:		base address
 *
 *	Search for the kernel VM area starting at @addr, and remove it.
 *	This function returns the found VM area, but using it is NOT safe
 *	on SMP machines, except for its size or flags.
 */
// ARM10C 20160820
// addr: 할당받은 page의 mmu에 반영된 가상주소
struct vm_struct *remove_vm_area(const void *addr)
{
	struct vmap_area *va;

	// addr: 할당받은 page의 mmu에 반영된 가상주소
	// find_vmap_area(할당받은 page의 mmu에 반영된 가상주소): 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
	va = find_vmap_area((unsigned long)addr);
	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소

	// find_vmap_area 에서 한일:
	// vmap_area_root.rb_node 에서 가지고 있는 rb tree의 주소를 기준으로
	// 할당받은 page의 mmu에 반영된 가상주소의 vmap_area 의 위치를 찾음

// 2016/8/20 종료
// 2016/8/27 시작

	// NOTE:
	// va 의 맵버 변수들 값을 정확히 알수 없음
	// va->flags: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->flags
	// 값에 VM_VM_AREA: 0x04 값이 있다고 가정하고 분석

	// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소,
	// va->flags: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->flags, VM_VM_AREA: 0x04
	if (va && va->flags & VM_VM_AREA) {
		// va->vm: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
		struct vm_struct *vm = va->vm;
		// vm: &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm

		spin_lock(&vmap_area_lock);

		// spin_lock 에서 한일:
		// &vmap_area_lock 을 이용한 spin lock 수행

		// va->vm: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
		va->vm = NULL;
		// va->vm: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm: NULL

		// va->flags: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->flags, VM_VM_AREA: 0x04
		va->flags &= ~VM_VM_AREA;
		// va->flags: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->flags: VM_VM_AREA 값이 umask 된 값

		spin_unlock(&vmap_area_lock);

		// spin_unlock 에서 한일:
		// &vmap_area_lock 을 이용한 spin unlock 수행

		// va->va_start: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_start,
		// va->va_end: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->va_end
		vmap_debug_free_range(va->va_start, va->va_end); // null function

		// va: 할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소
		free_unmap_vmap_area(va);

		// free_unmap_vmap_area 에서 한일:
		// cache 에 있는 변화된 값을 실제 메모리에 전부 반영
		// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함
		// free 되는 page 수를 계산하여 vmap_lazy_nr 에 더함
		// vmap_lazy_nr 이 0x2000 개가 넘을 경우 purge를 수행

		// va->size: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->size, PAGE_SIZE: 0x1000
		vm->size -= PAGE_SIZE;
		// va->size: (할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->size: 0x1000 를 뺀 값

		// vm: &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
		return vm;
		// return &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
	}
	return NULL;
}

// ARM10C 20160820
// addr: 할당받은 page의 mmu에 반영된 가상주소, 0
static void __vunmap(const void *addr, int deallocate_pages)
{
	struct vm_struct *area;

	// addr: 할당받은 page의 mmu에 반영된 가상주소
	if (!addr)
		return;

	// addr: 할당받은 page의 mmu에 반영된 가상주소
	if (WARN(!PAGE_ALIGNED(addr), "Trying to vfree() bad address (%p)\n",
			addr))
		return;

	// addr: 할당받은 page의 mmu에 반영된 가상주소
	// remove_vm_area(할당받은 page의 mmu에 반영된 가상주소): &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
	area = remove_vm_area(addr);
	// area: &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm

	// remove_vm_area 에서 한일:
	// vmap_area_root.rb_node 에서 가지고 있는 rb tree의 주소를 기준으로
	// 할당받은 page의 mmu에 반영된 가상주소의 vmap_area 의 위치를 찾음
	// cache 에 있는 변화된 값을 실제 메모리에 전부 반영
	// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함
	// free 되는 page 수를 계산하여 vmap_lazy_nr 에 더함
	// vmap_lazy_nr 이 0x2000 개가 넘을 경우 purge를 수행

	// area: &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
	if (unlikely(!area)) {
		WARN(1, KERN_ERR "Trying to vfree() nonexistent vm area (%p)\n",
				addr);
		return;
	}

	// addr: 할당받은 page의 mmu에 반영된 가상주소,
	// area->size: (&(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm)->size
	debug_check_no_locks_freed(addr, area->size); // null function

	// addr: 할당받은 page의 mmu에 반영된 가상주소,
	// area->size: (&(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm)->size
	debug_check_no_obj_freed(addr, area->size); // null function

	// deallocate_pages: 0
	if (deallocate_pages) {
		int i;

		for (i = 0; i < area->nr_pages; i++) {
			struct page *page = area->pages[i];

			BUG_ON(!page);
			__free_page(page);
		}

		if (area->flags & VM_VPAGES)
			vfree(area->pages);
		else
			kfree(area->pages);
	}

	// area: &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm
	kfree(area);

	// kfree 에서 한일:
	// &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm 의 page 주소를 구하고, 등록된 kmem_cache 주소를 찾음
	// &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm 의 object 을 등록된 kmem_cache 를 이용하여 free 하도록 함

	return;
	// return
}
 
/**
 *	vfree  -  release memory allocated by vmalloc()
 *	@addr:		memory base address
 *
 *	Free the virtually continuous memory area starting at @addr, as
 *	obtained from vmalloc(), vmalloc_32() or __vmalloc(). If @addr is
 *	NULL, no operation is performed.
 *
 *	Must not be called in NMI context (strictly speaking, only if we don't
 *	have CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG, but making the calling
 *	conventions for vfree() arch-depenedent would be a really bad idea)
 *
 *	NOTE: assumes that the object at *addr has a size >= sizeof(llist_node)
 */
void vfree(const void *addr)
{
	BUG_ON(in_nmi());

	kmemleak_free(addr);

	if (!addr)
		return;
	if (unlikely(in_interrupt())) {
		struct vfree_deferred *p = &__get_cpu_var(vfree_deferred);
		if (llist_add((struct llist_node *)addr, &p->list))
			schedule_work(&p->wq);
	} else
		__vunmap(addr, 1);
}
EXPORT_SYMBOL(vfree);

/**
 *	vunmap  -  release virtual mapping obtained by vmap()
 *	@addr:		memory base address
 *
 *	Free the virtually contiguous memory area starting at @addr,
 *	which was created from the page array passed to vmap().
 *
 *	Must not be called in interrupt context.
 */
// ARM10C 20160820
// p1: 할당받은 page의 mmu에 반영된 가상주소
void vunmap(const void *addr)
{
	// in_interrupt(): 0
	BUG_ON(in_interrupt());
	might_sleep(); // null function

	// addr: 할당받은 page의 mmu에 반영된 가상주소
	if (addr)
		// addr: 할당받은 page의 mmu에 반영된 가상주소
		__vunmap(addr, 0);

		// __vunmap 에서 한일:
		// vmap_area_root.rb_node 에서 가지고 있는 rb tree의 주소를 기준으로
		// 할당받은 page의 mmu에 반영된 가상주소의 vmap_area 의 위치를 찾음
		// cache 에 있는 변화된 값을 실제 메모리에 전부 반영
		// 가상주소에 매핑 되어 있는 pte 에 값을 0 으로 초기화 함
		// free 되는 page 수를 계산하여 vmap_lazy_nr 에 더함
		// vmap_lazy_nr 이 0x2000 개가 넘을 경우 purge를 수행
		// &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm 의 page 주소를 구하고, 등록된 kmem_cache 주소를 찾음
		// &(할당받은 page의 mmu에 반영된 가상주소 가 포함된 vmap_area 주소)->vm 의 object 을 등록된 kmem_cache 를 이용하여 free 하도록 함
}
EXPORT_SYMBOL(vunmap);

/**
 *	vmap  -  map an array of pages into virtually contiguous space
 *	@pages:		array of page pointers
 *	@count:		number of pages to map
 *	@flags:		vm_area->flags
 *	@prot:		page protection for the mapping
 *
 *	Maps @count pages from @pages into contiguous kernel virtual
 *	space.
 */
// ARM10C 20160813
// page: &(page 1개(4K)의 할당된 메모리 주소), 1, VM_IOREMAP: 0x00000001, prot: pgprot_kernel에 0x204 를 or 한 값
void *vmap(struct page **pages, unsigned int count,
		unsigned long flags, pgprot_t prot)
{
	struct vm_struct *area;

	might_sleep();

	// count: 1, totalram_pages: 총 free된 page 수
	if (count > totalram_pages)
		return NULL;

	// count: 1, PAGE_SHIFT: 12, flags: 0x00000001
	// get_vm_area_caller(0x1000, 0x00000001, __builtin_return_address(0)): kmem_cache#30-oX (vm_struct)
	area = get_vm_area_caller((count << PAGE_SHIFT), flags,
					__builtin_return_address(0));
	// area: kmem_cache#30-oX (vm_struct)

	// area: kmem_cache#30-oX (vm_struct)
	if (!area)
		return NULL;

	// area: kmem_cache#30-oX (vm_struct), prot: pgprot_kernel에 0x204 를 or 한 값, pages: &(page 1개(4K)의 할당된 메모리 주소)
	// map_vm_area(kmem_cache#30-oX (vm_struct), pgprot_kernel에 0x204 를 or 한 값, &(page 1개(4K)의 할당된 메모리 주소)): 0
	if (map_vm_area(area, prot, &pages)) {
		vunmap(area->addr);
		return NULL;
	}

	// map_vm_area 에서 한일:
	// 할당 받은 가상 주소값을 가지고 있는 page table section 하위 pte table을 갱신함
	// cache의 값을 전부 메모리에 반영

	// area->addr: (kmem_cache#30-oX (vm_struct))->addr: 할당받은 page의 가상주소
	return area->addr;
	// return 할당받은 page의 mmu에 반영된 가상주소
}
EXPORT_SYMBOL(vmap);

static void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, pgprot_t prot,
			    int node, const void *caller);
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
				 pgprot_t prot, int node)
{
	const int order = 0;
	struct page **pages;
	unsigned int nr_pages, array_size, i;
	gfp_t nested_gfp = (gfp_mask & GFP_RECLAIM_MASK) | __GFP_ZERO;

	nr_pages = get_vm_area_size(area) >> PAGE_SHIFT;
	array_size = (nr_pages * sizeof(struct page *));

	area->nr_pages = nr_pages;
	/* Please note that the recursion is strictly bounded. */
	if (array_size > PAGE_SIZE) {
		pages = __vmalloc_node(array_size, 1, nested_gfp|__GFP_HIGHMEM,
				PAGE_KERNEL, node, area->caller);
		area->flags |= VM_VPAGES;
	} else {
		pages = kmalloc_node(array_size, nested_gfp, node);
	}
	area->pages = pages;
	if (!area->pages) {
		remove_vm_area(area->addr);
		kfree(area);
		return NULL;
	}

	for (i = 0; i < area->nr_pages; i++) {
		struct page *page;
		gfp_t tmp_mask = gfp_mask | __GFP_NOWARN;

		if (node == NUMA_NO_NODE)
			page = alloc_page(tmp_mask);
		else
			page = alloc_pages_node(node, tmp_mask, order);

		if (unlikely(!page)) {
			/* Successfully allocated i pages, free them in __vunmap() */
			area->nr_pages = i;
			goto fail;
		}
		area->pages[i] = page;
	}

	if (map_vm_area(area, prot, &pages))
		goto fail;
	return area->addr;

fail:
	warn_alloc_failed(gfp_mask, order,
			  "vmalloc: allocation failure, allocated %ld of %ld bytes\n",
			  (area->nr_pages*PAGE_SIZE), area->size);
	vfree(area->addr);
	return NULL;
}

/**
 *	__vmalloc_node_range  -  allocate virtually contiguous memory
 *	@size:		allocation size
 *	@align:		desired alignment
 *	@start:		vm area range start
 *	@end:		vm area range end
 *	@gfp_mask:	flags for the page level allocator
 *	@prot:		protection mask for the allocated pages
 *	@node:		node to use for allocation or NUMA_NO_NODE
 *	@caller:	caller's return address
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator with @gfp_mask flags.  Map them into contiguous
 *	kernel virtual space, using a pagetable protection of @prot.
 */
void *__vmalloc_node_range(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, int node, const void *caller)
{
	struct vm_struct *area;
	void *addr;
	unsigned long real_size = size;

	size = PAGE_ALIGN(size);
	if (!size || (size >> PAGE_SHIFT) > totalram_pages)
		goto fail;

	area = __get_vm_area_node(size, align, VM_ALLOC | VM_UNINITIALIZED,
				  start, end, node, gfp_mask, caller);
	if (!area)
		goto fail;

	addr = __vmalloc_area_node(area, gfp_mask, prot, node);
	if (!addr)
		return NULL;

	/*
	 * In this function, newly allocated vm_struct has VM_UNINITIALIZED
	 * flag. It means that vm_struct is not fully initialized.
	 * Now, it is fully initialized, so remove this flag here.
	 */
	clear_vm_uninitialized_flag(area);

	/*
	 * A ref_count = 2 is needed because vm_struct allocated in
	 * __get_vm_area_node() contains a reference to the virtual address of
	 * the vmalloc'ed block.
	 */
	kmemleak_alloc(addr, real_size, 2, gfp_mask);

	return addr;

fail:
	warn_alloc_failed(gfp_mask, 0,
			  "vmalloc: allocation failure: %lu bytes\n",
			  real_size);
	return NULL;
}

/**
 *	__vmalloc_node  -  allocate virtually contiguous memory
 *	@size:		allocation size
 *	@align:		desired alignment
 *	@gfp_mask:	flags for the page level allocator
 *	@prot:		protection mask for the allocated pages
 *	@node:		node to use for allocation or NUMA_NO_NODE
 *	@caller:	caller's return address
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator with @gfp_mask flags.  Map them into contiguous
 *	kernel virtual space, using a pagetable protection of @prot.
 */
static void *__vmalloc_node(unsigned long size, unsigned long align,
			    gfp_t gfp_mask, pgprot_t prot,
			    int node, const void *caller)
{
	return __vmalloc_node_range(size, align, VMALLOC_START, VMALLOC_END,
				gfp_mask, prot, node, caller);
}

void *__vmalloc(unsigned long size, gfp_t gfp_mask, pgprot_t prot)
{
	return __vmalloc_node(size, 1, gfp_mask, prot, NUMA_NO_NODE,
				__builtin_return_address(0));
}
EXPORT_SYMBOL(__vmalloc);

static inline void *__vmalloc_node_flags(unsigned long size,
					int node, gfp_t flags)
{
	return __vmalloc_node(size, 1, flags, PAGE_KERNEL,
					node, __builtin_return_address(0));
}

/**
 *	vmalloc  -  allocate virtually contiguous memory
 *	@size:		allocation size
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc(unsigned long size)
{
	return __vmalloc_node_flags(size, NUMA_NO_NODE,
				    GFP_KERNEL | __GFP_HIGHMEM);
}
EXPORT_SYMBOL(vmalloc);

/**
 *	vzalloc - allocate virtually contiguous memory with zero fill
 *	@size:	allocation size
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *	The memory allocated is set to zero.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vzalloc(unsigned long size)
{
	return __vmalloc_node_flags(size, NUMA_NO_NODE,
				GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc);

/**
 * vmalloc_user - allocate zeroed virtually contiguous memory for userspace
 * @size: allocation size
 *
 * The resulting memory area is zeroed so it can be mapped to userspace
 * without leaking data.
 */
void *vmalloc_user(unsigned long size)
{
	struct vm_struct *area;
	void *ret;

	ret = __vmalloc_node(size, SHMLBA,
			     GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO,
			     PAGE_KERNEL, NUMA_NO_NODE,
			     __builtin_return_address(0));
	if (ret) {
		area = find_vm_area(ret);
		area->flags |= VM_USERMAP;
	}
	return ret;
}
EXPORT_SYMBOL(vmalloc_user);

/**
 *	vmalloc_node  -  allocate memory on a specific node
 *	@size:		allocation size
 *	@node:		numa node
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into contiguous kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc_node(unsigned long size, int node)
{
	return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL,
					node, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_node);

/**
 * vzalloc_node - allocate memory on a specific node with zero fill
 * @size:	allocation size
 * @node:	numa node
 *
 * Allocate enough pages to cover @size from the page level
 * allocator and map them into contiguous kernel virtual space.
 * The memory allocated is set to zero.
 *
 * For tight control over page level allocator and protection flags
 * use __vmalloc_node() instead.
 */
void *vzalloc_node(unsigned long size, int node)
{
	return __vmalloc_node_flags(size, node,
			 GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
}
EXPORT_SYMBOL(vzalloc_node);

#ifndef PAGE_KERNEL_EXEC
# define PAGE_KERNEL_EXEC PAGE_KERNEL
#endif

/**
 *	vmalloc_exec  -  allocate virtually contiguous, executable memory
 *	@size:		allocation size
 *
 *	Kernel-internal function to allocate enough pages to cover @size
 *	the page level allocator and map them into contiguous and
 *	executable kernel virtual space.
 *
 *	For tight control over page level allocator and protection flags
 *	use __vmalloc() instead.
 */

void *vmalloc_exec(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL_EXEC,
			      NUMA_NO_NODE, __builtin_return_address(0));
}

#if defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA32)
#define GFP_VMALLOC32 GFP_DMA32 | GFP_KERNEL
#elif defined(CONFIG_64BIT) && defined(CONFIG_ZONE_DMA)
#define GFP_VMALLOC32 GFP_DMA | GFP_KERNEL
#else
#define GFP_VMALLOC32 GFP_KERNEL
#endif

/**
 *	vmalloc_32  -  allocate virtually contiguous memory (32bit addressable)
 *	@size:		allocation size
 *
 *	Allocate enough 32bit PA addressable pages to cover @size from the
 *	page level allocator and map them into contiguous kernel virtual space.
 */
void *vmalloc_32(unsigned long size)
{
	return __vmalloc_node(size, 1, GFP_VMALLOC32, PAGE_KERNEL,
			      NUMA_NO_NODE, __builtin_return_address(0));
}
EXPORT_SYMBOL(vmalloc_32);

/**
 * vmalloc_32_user - allocate zeroed virtually contiguous 32bit memory
 *	@size:		allocation size
 *
 * The resulting memory area is 32bit addressable and zeroed so it can be
 * mapped to userspace without leaking data.
 */
void *vmalloc_32_user(unsigned long size)
{
	struct vm_struct *area;
	void *ret;

	ret = __vmalloc_node(size, 1, GFP_VMALLOC32 | __GFP_ZERO, PAGE_KERNEL,
			     NUMA_NO_NODE, __builtin_return_address(0));
	if (ret) {
		area = find_vm_area(ret);
		area->flags |= VM_USERMAP;
	}
	return ret;
}
EXPORT_SYMBOL(vmalloc_32_user);

/*
 * small helper routine , copy contents to buf from addr.
 * If the page is not present, fill zero.
 */

static int aligned_vread(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = (unsigned long)addr & ~PAGE_MASK;
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calles for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/*
			 * we can expect USER0 is not used (see vread/vwrite's
			 * function description)
			 */
			void *map = kmap_atomic(p);
			memcpy(buf, map + offset, length);
			kunmap_atomic(map);
		} else
			memset(buf, 0, length);

		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

static int aligned_vwrite(char *buf, char *addr, unsigned long count)
{
	struct page *p;
	int copied = 0;

	while (count) {
		unsigned long offset, length;

		offset = (unsigned long)addr & ~PAGE_MASK;
		length = PAGE_SIZE - offset;
		if (length > count)
			length = count;
		p = vmalloc_to_page(addr);
		/*
		 * To do safe access to this _mapped_ area, we need
		 * lock. But adding lock here means that we need to add
		 * overhead of vmalloc()/vfree() calles for this _debug_
		 * interface, rarely used. Instead of that, we'll use
		 * kmap() and get small overhead in this access function.
		 */
		if (p) {
			/*
			 * we can expect USER0 is not used (see vread/vwrite's
			 * function description)
			 */
			void *map = kmap_atomic(p);
			memcpy(map + offset, buf, length);
			kunmap_atomic(map);
		}
		addr += length;
		buf += length;
		copied += length;
		count -= length;
	}
	return copied;
}

/**
 *	vread() -  read vmalloc area in a safe way.
 *	@buf:		buffer for reading data
 *	@addr:		vm address.
 *	@count:		number of bytes to be read.
 *
 *	Returns # of bytes which addr and buf should be increased.
 *	(same number to @count). Returns 0 if [addr...addr+count) doesn't
 *	includes any intersect with alive vmalloc area.
 *
 *	This function checks that addr is a valid vmalloc'ed area, and
 *	copy data from that area to a given buffer. If the given memory range
 *	of [addr...addr+count) includes some valid address, data is copied to
 *	proper area of @buf. If there are memory holes, they'll be zero-filled.
 *	IOREMAP area is treated as memory hole and no copy is done.
 *
 *	If [addr...addr+count) doesn't includes any intersects with alive
 *	vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 *	Note: In usual ops, vread() is never necessary because the caller
 *	should know vmalloc() area is valid and can use memcpy().
 *	This is for routines which have to access vmalloc area without
 *	any informaion, as /dev/kmem.
 *
 */

long vread(char *buf, char *addr, unsigned long count)
{
	struct vmap_area *va;
	struct vm_struct *vm;
	char *vaddr, *buf_start = buf;
	unsigned long buflen = count;
	unsigned long n;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	spin_lock(&vmap_area_lock);
	list_for_each_entry(va, &vmap_area_list, list) {
		if (!count)
			break;

		if (!(va->flags & VM_VM_AREA))
			continue;

		vm = va->vm;
		vaddr = (char *) vm->addr;
		if (addr >= vaddr + get_vm_area_size(vm))
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			*buf = '\0';
			buf++;
			addr++;
			count--;
		}
		n = vaddr + get_vm_area_size(vm) - addr;
		if (n > count)
			n = count;
		if (!(vm->flags & VM_IOREMAP))
			aligned_vread(buf, addr, n);
		else /* IOREMAP area is treated as memory hole */
			memset(buf, 0, n);
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	spin_unlock(&vmap_area_lock);

	if (buf == buf_start)
		return 0;
	/* zero-fill memory holes */
	if (buf != buf_start + buflen)
		memset(buf, 0, buflen - (buf - buf_start));

	return buflen;
}

/**
 *	vwrite() -  write vmalloc area in a safe way.
 *	@buf:		buffer for source data
 *	@addr:		vm address.
 *	@count:		number of bytes to be read.
 *
 *	Returns # of bytes which addr and buf should be incresed.
 *	(same number to @count).
 *	If [addr...addr+count) doesn't includes any intersect with valid
 *	vmalloc area, returns 0.
 *
 *	This function checks that addr is a valid vmalloc'ed area, and
 *	copy data from a buffer to the given addr. If specified range of
 *	[addr...addr+count) includes some valid address, data is copied from
 *	proper area of @buf. If there are memory holes, no copy to hole.
 *	IOREMAP area is treated as memory hole and no copy is done.
 *
 *	If [addr...addr+count) doesn't includes any intersects with alive
 *	vm_struct area, returns 0. @buf should be kernel's buffer.
 *
 *	Note: In usual ops, vwrite() is never necessary because the caller
 *	should know vmalloc() area is valid and can use memcpy().
 *	This is for routines which have to access vmalloc area without
 *	any informaion, as /dev/kmem.
 */

long vwrite(char *buf, char *addr, unsigned long count)
{
	struct vmap_area *va;
	struct vm_struct *vm;
	char *vaddr;
	unsigned long n, buflen;
	int copied = 0;

	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;
	buflen = count;

	spin_lock(&vmap_area_lock);
	list_for_each_entry(va, &vmap_area_list, list) {
		if (!count)
			break;

		if (!(va->flags & VM_VM_AREA))
			continue;

		vm = va->vm;
		vaddr = (char *) vm->addr;
		if (addr >= vaddr + get_vm_area_size(vm))
			continue;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			buf++;
			addr++;
			count--;
		}
		n = vaddr + get_vm_area_size(vm) - addr;
		if (n > count)
			n = count;
		if (!(vm->flags & VM_IOREMAP)) {
			aligned_vwrite(buf, addr, n);
			copied++;
		}
		buf += n;
		addr += n;
		count -= n;
	}
finished:
	spin_unlock(&vmap_area_lock);
	if (!copied)
		return 0;
	return buflen;
}

/**
 *	remap_vmalloc_range_partial  -  map vmalloc pages to userspace
 *	@vma:		vma to cover
 *	@uaddr:		target user address to start at
 *	@kaddr:		virtual address of vmalloc kernel memory
 *	@size:		size of map area
 *
 *	Returns:	0 for success, -Exxx on failure
 *
 *	This function checks that @kaddr is a valid vmalloc'ed area,
 *	and that it is big enough to cover the range starting at
 *	@uaddr in @vma. Will return failure if that criteria isn't
 *	met.
 *
 *	Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range_partial(struct vm_area_struct *vma, unsigned long uaddr,
				void *kaddr, unsigned long size)
{
	struct vm_struct *area;

	size = PAGE_ALIGN(size);

	if (!PAGE_ALIGNED(uaddr) || !PAGE_ALIGNED(kaddr))
		return -EINVAL;

	area = find_vm_area(kaddr);
	if (!area)
		return -EINVAL;

	if (!(area->flags & VM_USERMAP))
		return -EINVAL;

	if (kaddr + size > area->addr + area->size)
		return -EINVAL;

	do {
		struct page *page = vmalloc_to_page(kaddr);
		int ret;

		ret = vm_insert_page(vma, uaddr, page);
		if (ret)
			return ret;

		uaddr += PAGE_SIZE;
		kaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	} while (size > 0);

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

	return 0;
}
EXPORT_SYMBOL(remap_vmalloc_range_partial);

/**
 *	remap_vmalloc_range  -  map vmalloc pages to userspace
 *	@vma:		vma to cover (map full range of vma)
 *	@addr:		vmalloc memory
 *	@pgoff:		number of pages into addr before first page to map
 *
 *	Returns:	0 for success, -Exxx on failure
 *
 *	This function checks that addr is a valid vmalloc'ed area, and
 *	that it is big enough to cover the vma. Will return failure if
 *	that criteria isn't met.
 *
 *	Similar to remap_pfn_range() (see mm/memory.c)
 */
int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
						unsigned long pgoff)
{
	return remap_vmalloc_range_partial(vma, vma->vm_start,
					   addr + (pgoff << PAGE_SHIFT),
					   vma->vm_end - vma->vm_start);
}
EXPORT_SYMBOL(remap_vmalloc_range);

/*
 * Implement a stub for vmalloc_sync_all() if the architecture chose not to
 * have one.
 */
void  __attribute__((weak)) vmalloc_sync_all(void)
{
}


static int f(pte_t *pte, pgtable_t table, unsigned long addr, void *data)
{
	pte_t ***p = data;

	if (p) {
		*(*p) = pte;
		(*p)++;
	}
	return 0;
}

/**
 *	alloc_vm_area - allocate a range of kernel address space
 *	@size:		size of the area
 *	@ptes:		returns the PTEs for the address space
 *
 *	Returns:	NULL on failure, vm_struct on success
 *
 *	This function reserves a range of kernel address space, and
 *	allocates pagetables to map that range.  No actual mappings
 *	are created.
 *
 *	If @ptes is non-NULL, pointers to the PTEs (in init_mm)
 *	allocated for the VM area are returned.
 */
struct vm_struct *alloc_vm_area(size_t size, pte_t **ptes)
{
	struct vm_struct *area;

	area = get_vm_area_caller(size, VM_IOREMAP,
				__builtin_return_address(0));
	if (area == NULL)
		return NULL;

	/*
	 * This ensures that page tables are constructed for this region
	 * of kernel virtual address space and mapped into init_mm.
	 */
	if (apply_to_page_range(&init_mm, (unsigned long)area->addr,
				size, f, ptes ? &ptes : NULL)) {
		free_vm_area(area);
		return NULL;
	}

	return area;
}
EXPORT_SYMBOL_GPL(alloc_vm_area);

void free_vm_area(struct vm_struct *area)
{
	struct vm_struct *ret;
	ret = remove_vm_area(area->addr);
	BUG_ON(ret != area);
	kfree(area);
}
EXPORT_SYMBOL_GPL(free_vm_area);

#ifdef CONFIG_SMP
static struct vmap_area *node_to_va(struct rb_node *n)
{
	return n ? rb_entry(n, struct vmap_area, rb_node) : NULL;
}

/**
 * pvm_find_next_prev - find the next and prev vmap_area surrounding @end
 * @end: target address
 * @pnext: out arg for the next vmap_area
 * @pprev: out arg for the previous vmap_area
 *
 * Returns: %true if either or both of next and prev are found,
 *	    %false if no vmap_area exists
 *
 * Find vmap_areas end addresses of which enclose @end.  ie. if not
 * NULL, *pnext->va_end > @end and *pprev->va_end <= @end.
 */
static bool pvm_find_next_prev(unsigned long end,
			       struct vmap_area **pnext,
			       struct vmap_area **pprev)
{
	struct rb_node *n = vmap_area_root.rb_node;
	struct vmap_area *va = NULL;

	while (n) {
		va = rb_entry(n, struct vmap_area, rb_node);
		if (end < va->va_end)
			n = n->rb_left;
		else if (end > va->va_end)
			n = n->rb_right;
		else
			break;
	}

	if (!va)
		return false;

	if (va->va_end > end) {
		*pnext = va;
		*pprev = node_to_va(rb_prev(&(*pnext)->rb_node));
	} else {
		*pprev = va;
		*pnext = node_to_va(rb_next(&(*pprev)->rb_node));
	}
	return true;
}

/**
 * pvm_determine_end - find the highest aligned address between two vmap_areas
 * @pnext: in/out arg for the next vmap_area
 * @pprev: in/out arg for the previous vmap_area
 * @align: alignment
 *
 * Returns: determined end address
 *
 * Find the highest aligned address between *@pnext and *@pprev below
 * VMALLOC_END.  *@pnext and *@pprev are adjusted so that the aligned
 * down address is between the end addresses of the two vmap_areas.
 *
 * Please note that the address returned by this function may fall
 * inside *@pnext vmap_area.  The caller is responsible for checking
 * that.
 */
static unsigned long pvm_determine_end(struct vmap_area **pnext,
				       struct vmap_area **pprev,
				       unsigned long align)
{
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	unsigned long addr;

	if (*pnext)
		addr = min((*pnext)->va_start & ~(align - 1), vmalloc_end);
	else
		addr = vmalloc_end;

	while (*pprev && (*pprev)->va_end > addr) {
		*pnext = *pprev;
		*pprev = node_to_va(rb_prev(&(*pnext)->rb_node));
	}

	return addr;
}

/**
 * pcpu_get_vm_areas - allocate vmalloc areas for percpu allocator
 * @offsets: array containing offset of each area
 * @sizes: array containing size of each area
 * @nr_vms: the number of areas to allocate
 * @align: alignment, all entries in @offsets and @sizes must be aligned to this
 *
 * Returns: kmalloc'd vm_struct pointer array pointing to allocated
 *	    vm_structs on success, %NULL on failure
 *
 * Percpu allocator wants to use congruent vm areas so that it can
 * maintain the offsets among percpu areas.  This function allocates
 * congruent vmalloc areas for it with GFP_KERNEL.  These areas tend to
 * be scattered pretty far, distance between two areas easily going up
 * to gigabytes.  To avoid interacting with regular vmallocs, these
 * areas are allocated from top.
 *
 * Despite its complicated look, this allocator is rather simple.  It
 * does everything top-down and scans areas from the end looking for
 * matching slot.  While scanning, if any of the areas overlaps with
 * existing vmap_area, the base address is pulled down to fit the
 * area.  Scanning is repeated till all the areas fit and then all
 * necessary data structres are inserted and the result is returned.
 */
struct vm_struct **pcpu_get_vm_areas(const unsigned long *offsets,
				     const size_t *sizes, int nr_vms,
				     size_t align)
{
	const unsigned long vmalloc_start = ALIGN(VMALLOC_START, align);
	const unsigned long vmalloc_end = VMALLOC_END & ~(align - 1);
	struct vmap_area **vas, *prev, *next;
	struct vm_struct **vms;
	int area, area2, last_area, term_area;
	unsigned long base, start, end, last_end;
	bool purged = false;

	/* verify parameters and allocate data structures */
	BUG_ON(align & ~PAGE_MASK || !is_power_of_2(align));
	for (last_area = 0, area = 0; area < nr_vms; area++) {
		start = offsets[area];
		end = start + sizes[area];

		/* is everything aligned properly? */
		BUG_ON(!IS_ALIGNED(offsets[area], align));
		BUG_ON(!IS_ALIGNED(sizes[area], align));

		/* detect the area with the highest address */
		if (start > offsets[last_area])
			last_area = area;

		for (area2 = 0; area2 < nr_vms; area2++) {
			unsigned long start2 = offsets[area2];
			unsigned long end2 = start2 + sizes[area2];

			if (area2 == area)
				continue;

			BUG_ON(start2 >= start && start2 < end);
			BUG_ON(end2 <= end && end2 > start);
		}
	}
	last_end = offsets[last_area] + sizes[last_area];

	if (vmalloc_end - vmalloc_start < last_end) {
		WARN_ON(true);
		return NULL;
	}

	vms = kcalloc(nr_vms, sizeof(vms[0]), GFP_KERNEL);
	vas = kcalloc(nr_vms, sizeof(vas[0]), GFP_KERNEL);
	if (!vas || !vms)
		goto err_free2;

	for (area = 0; area < nr_vms; area++) {
		vas[area] = kzalloc(sizeof(struct vmap_area), GFP_KERNEL);
		vms[area] = kzalloc(sizeof(struct vm_struct), GFP_KERNEL);
		if (!vas[area] || !vms[area])
			goto err_free;
	}
retry:
	spin_lock(&vmap_area_lock);

	/* start scanning - we scan from the top, begin with the last area */
	area = term_area = last_area;
	start = offsets[area];
	end = start + sizes[area];

	if (!pvm_find_next_prev(vmap_area_pcpu_hole, &next, &prev)) {
		base = vmalloc_end - last_end;
		goto found;
	}
	base = pvm_determine_end(&next, &prev, align) - end;

	while (true) {
		BUG_ON(next && next->va_end <= base + end);
		BUG_ON(prev && prev->va_end > base + end);

		/*
		 * base might have underflowed, add last_end before
		 * comparing.
		 */
		if (base + last_end < vmalloc_start + last_end) {
			spin_unlock(&vmap_area_lock);
			if (!purged) {
				purge_vmap_area_lazy();
				purged = true;
				goto retry;
			}
			goto err_free;
		}

		/*
		 * If next overlaps, move base downwards so that it's
		 * right below next and then recheck.
		 */
		if (next && next->va_start < base + end) {
			base = pvm_determine_end(&next, &prev, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * If prev overlaps, shift down next and prev and move
		 * base so that it's right below new next and then
		 * recheck.
		 */
		if (prev && prev->va_end > base + start)  {
			next = prev;
			prev = node_to_va(rb_prev(&next->rb_node));
			base = pvm_determine_end(&next, &prev, align) - end;
			term_area = area;
			continue;
		}

		/*
		 * This area fits, move on to the previous one.  If
		 * the previous one is the terminal one, we're done.
		 */
		area = (area + nr_vms - 1) % nr_vms;
		if (area == term_area)
			break;
		start = offsets[area];
		end = start + sizes[area];
		pvm_find_next_prev(base + end, &next, &prev);
	}
found:
	/* we've found a fitting base, insert all va's */
	for (area = 0; area < nr_vms; area++) {
		struct vmap_area *va = vas[area];

		va->va_start = base + offsets[area];
		va->va_end = va->va_start + sizes[area];
		__insert_vmap_area(va);
	}

	vmap_area_pcpu_hole = base + offsets[last_area];

	spin_unlock(&vmap_area_lock);

	/* insert all vm's */
	for (area = 0; area < nr_vms; area++)
		setup_vmalloc_vm(vms[area], vas[area], VM_ALLOC,
				 pcpu_get_vm_areas);

	kfree(vas);
	return vms;

err_free:
	for (area = 0; area < nr_vms; area++) {
		kfree(vas[area]);
		kfree(vms[area]);
	}
err_free2:
	kfree(vas);
	kfree(vms);
	return NULL;
}

/**
 * pcpu_free_vm_areas - free vmalloc areas for percpu allocator
 * @vms: vm_struct pointer array returned by pcpu_get_vm_areas()
 * @nr_vms: the number of allocated areas
 *
 * Free vm_structs and the array allocated by pcpu_get_vm_areas().
 */
void pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms)
{
	int i;

	for (i = 0; i < nr_vms; i++)
		free_vm_area(vms[i]);
	kfree(vms);
}
#endif	/* CONFIG_SMP */

#ifdef CONFIG_PROC_FS
static void *s_start(struct seq_file *m, loff_t *pos)
	__acquires(&vmap_area_lock)
{
	loff_t n = *pos;
	struct vmap_area *va;

	spin_lock(&vmap_area_lock);
	va = list_entry((&vmap_area_list)->next, typeof(*va), list);
	while (n > 0 && &va->list != &vmap_area_list) {
		n--;
		va = list_entry(va->list.next, typeof(*va), list);
	}
	if (!n && &va->list != &vmap_area_list)
		return va;

	return NULL;

}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct vmap_area *va = p, *next;

	++*pos;
	next = list_entry(va->list.next, typeof(*va), list);
	if (&next->list != &vmap_area_list)
		return next;

	return NULL;
}

static void s_stop(struct seq_file *m, void *p)
	__releases(&vmap_area_lock)
{
	spin_unlock(&vmap_area_lock);
}

static void show_numa_info(struct seq_file *m, struct vm_struct *v)
{
	if (IS_ENABLED(CONFIG_NUMA)) {
		unsigned int nr, *counters = m->private;

		if (!counters)
			return;

		/* Pair with smp_wmb() in clear_vm_uninitialized_flag() */
		smp_rmb();
		if (v->flags & VM_UNINITIALIZED)
			return;

		memset(counters, 0, nr_node_ids * sizeof(unsigned int));

		for (nr = 0; nr < v->nr_pages; nr++)
			counters[page_to_nid(v->pages[nr])]++;

		for_each_node_state(nr, N_HIGH_MEMORY)
			if (counters[nr])
				seq_printf(m, " N%u=%u", nr, counters[nr]);
	}
}

static int s_show(struct seq_file *m, void *p)
{
	struct vmap_area *va = p;
	struct vm_struct *v;

	/*
	 * s_show can encounter race with remove_vm_area, !VM_VM_AREA on
	 * behalf of vmap area is being tear down or vm_map_ram allocation.
	 */
	if (!(va->flags & VM_VM_AREA))
		return 0;

	v = va->vm;

	seq_printf(m, "0x%pK-0x%pK %7ld",
		v->addr, v->addr + v->size, v->size);

	if (v->caller)
		seq_printf(m, " %pS", v->caller);

	if (v->nr_pages)
		seq_printf(m, " pages=%d", v->nr_pages);

	if (v->phys_addr)
		seq_printf(m, " phys=%llx", (unsigned long long)v->phys_addr);

	if (v->flags & VM_IOREMAP)
		seq_printf(m, " ioremap");

	if (v->flags & VM_ALLOC)
		seq_printf(m, " vmalloc");

	if (v->flags & VM_MAP)
		seq_printf(m, " vmap");

	if (v->flags & VM_USERMAP)
		seq_printf(m, " user");

	if (v->flags & VM_VPAGES)
		seq_printf(m, " vpages");

	show_numa_info(m, v);
	seq_putc(m, '\n');
	return 0;
}

static const struct seq_operations vmalloc_op = {
	.start = s_start,
	.next = s_next,
	.stop = s_stop,
	.show = s_show,
};

static int vmalloc_open(struct inode *inode, struct file *file)
{
	unsigned int *ptr = NULL;
	int ret;

	if (IS_ENABLED(CONFIG_NUMA)) {
		ptr = kmalloc(nr_node_ids * sizeof(unsigned int), GFP_KERNEL);
		if (ptr == NULL)
			return -ENOMEM;
	}
	ret = seq_open(file, &vmalloc_op);
	if (!ret) {
		struct seq_file *m = file->private_data;
		m->private = ptr;
	} else
		kfree(ptr);
	return ret;
}

static const struct file_operations proc_vmalloc_operations = {
	.open		= vmalloc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

static int __init proc_vmalloc_init(void)
{
	proc_create("vmallocinfo", S_IRUSR, NULL, &proc_vmalloc_operations);
	return 0;
}
module_init(proc_vmalloc_init);

void get_vmalloc_info(struct vmalloc_info *vmi)
{
	struct vmap_area *va;
	unsigned long free_area_size;
	unsigned long prev_end;

	vmi->used = 0;
	vmi->largest_chunk = 0;

	prev_end = VMALLOC_START;

	spin_lock(&vmap_area_lock);

	if (list_empty(&vmap_area_list)) {
		vmi->largest_chunk = VMALLOC_TOTAL;
		goto out;
	}

	list_for_each_entry(va, &vmap_area_list, list) {
		unsigned long addr = va->va_start;

		/*
		 * Some archs keep another range for modules in vmalloc space
		 */
		if (addr < VMALLOC_START)
			continue;
		if (addr >= VMALLOC_END)
			break;

		if (va->flags & (VM_LAZY_FREE | VM_LAZY_FREEING))
			continue;

		vmi->used += (va->va_end - va->va_start);

		free_area_size = addr - prev_end;
		if (vmi->largest_chunk < free_area_size)
			vmi->largest_chunk = free_area_size;

		prev_end = va->va_end;
	}

	if (VMALLOC_END - prev_end > vmi->largest_chunk)
		vmi->largest_chunk = VMALLOC_END - prev_end;

out:
	spin_unlock(&vmap_area_lock);
}
#endif

