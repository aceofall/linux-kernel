#ifndef __ASM_GENERIC_GETORDER_H
#define __ASM_GENERIC_GETORDER_H

#ifndef __ASSEMBLY__

#include <linux/compiler.h>
#include <linux/log2.h>

/*
 * Runtime evaluation of get_order()
 */
// ARM10C 20140419
// size: 64
static inline __attribute_const__
int __get_order(unsigned long size)
{
	int order;

	// size: 64
	size--;
	// size: 63

	// size: 63, PAGE_SHIFT: 12
	size >>= PAGE_SHIFT;
	// size : 0

#if BITS_PER_LONG == 32 // BITS_PER_LONG: 32
	// size : 0
	order = fls(size);
	// order: 0
#else
	order = fls64(size);
#endif
	// order: 0
	return order;
	// return 0
}

/**
 * get_order - Determine the allocation order of a memory size
 * @size: The size for which to get the order
 *
 * Determine the allocation order of a particular sized block of memory.  This
 * is on a logarithmic scale, where:
 *
 *	0 -> 2^0 * PAGE_SIZE and below
 *	1 -> 2^1 * PAGE_SIZE to 2^0 * PAGE_SIZE + 1
 *	2 -> 2^2 * PAGE_SIZE to 2^1 * PAGE_SIZE + 1
 *	3 -> 2^3 * PAGE_SIZE to 2^2 * PAGE_SIZE + 1
 *	4 -> 2^4 * PAGE_SIZE to 2^3 * PAGE_SIZE + 1
 *	...
 *
 * The order returned is used to find the smallest allocation granule required
 * to hold an object of the specified size.
 *
 * The result is undefined if the size is 0.
 *
 * This function may be used to initialise variables with compile time
 * evaluations of constants.
 */
// ARM10C 20140419
#define get_order(n)						\
(								\
	__builtin_constant_p(n) ? (				\
		((n) == 0UL) ? BITS_PER_LONG - PAGE_SHIFT :	\
		(((n) < (1UL << PAGE_SHIFT)) ? 0 :		\
		 ilog2((n) - 1) - PAGE_SHIFT + 1)		\
	) :							\
	__get_order(n)						\
)

#endif	/* __ASSEMBLY__ */

#endif	/* __ASM_GENERIC_GETORDER_H */
