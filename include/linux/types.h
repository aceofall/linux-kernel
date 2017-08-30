#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H

#define __EXPORTED_HEADERS__
#include <uapi/linux/types.h>

#ifndef __ASSEMBLY__

// ARM10C 20140215
// cpu_possible_bits[1]
// ARM10C 20140913
// cpu_active_bits[1]
// ARM10C 20140920
// DECLARE_BITMAP(bitmap, 0x100): bitmap[8]
// ARM10C 20140927
// cpu_online_bits[1]
// ARM10C 20141004
// IRQ_BITMAP_BITS: 8212
// DECLARE_BITMAP(allocated_irqs, 8212): allocated_irqs[257]
// ARM10C 20151114
// DECLARE_BITMAP(bits, 1): bits[1]
// ARM10C 20160604
// ARM10C 20170201
#define DECLARE_BITMAP(name,bits) \
	unsigned long name[BITS_TO_LONGS(bits)]

// ARM10C 20151003
// ARM10C 20151114
// ARM10C 20160521
typedef __u32 __kernel_dev_t;

typedef __kernel_fd_set		fd_set;
// ARM10C 20151003
// ARM10C 20151114
// ARM10C 20160521
typedef __kernel_dev_t		dev_t;
typedef __kernel_ino_t		ino_t;
typedef __kernel_mode_t		mode_t;
// ARM10C 20151003
// ARM10C 20151031
// ARM10C 20160116
// ARM10C 20160319
// ARM10C 20160604
// ARM10C 20160730
typedef unsigned short		umode_t;
// ARM10C 20160604
typedef __u32			nlink_t;
typedef __kernel_off_t		off_t;
// ARM10C 20150919
// ARM10C 20161203
// ARM10C 20161217
typedef __kernel_pid_t		pid_t;
typedef __kernel_daddr_t	daddr_t;
typedef __kernel_key_t		key_t;
typedef __kernel_suseconds_t	suseconds_t;
typedef __kernel_timer_t	timer_t;
typedef __kernel_clockid_t	clockid_t;
typedef __kernel_mqd_t		mqd_t;

typedef _Bool			bool;

// ARM10C 20150919
// ARM10C 20151003
// ARM10C 20151128
// ARM10C 20160319
// ARM10C 20160604
typedef __kernel_uid32_t	uid_t;
// ARM10C 20151003
// ARM10C 20160319
// ARM10C 20160604
typedef __kernel_gid32_t	gid_t;
typedef __kernel_uid16_t        uid16_t;
typedef __kernel_gid16_t        gid16_t;

typedef unsigned long		uintptr_t;

#ifdef CONFIG_UID16
/* This is defined by include/asm-{arch}/posix_types.h */
typedef __kernel_old_uid_t	old_uid_t;
typedef __kernel_old_gid_t	old_gid_t;
#endif /* CONFIG_UID16 */

#if defined(__GNUC__)
// ARM10C 20151003
// ARM10C 20151114
// ARM10C 20160319
// ARM10C 20160604
typedef __kernel_loff_t		loff_t;
#endif

/*
 * The following typedefs are also protected by individual ifdefs for
 * historical reasons:
 */
#ifndef _SIZE_T
#define _SIZE_T
// ARM10C 20140607
// ARM10C 20141025
typedef __kernel_size_t		size_t;
#endif

#ifndef _SSIZE_T
#define _SSIZE_T
typedef __kernel_ssize_t	ssize_t;
#endif

#ifndef _PTRDIFF_T
#define _PTRDIFF_T
typedef __kernel_ptrdiff_t	ptrdiff_t;
#endif

#ifndef _TIME_T
#define _TIME_T
typedef __kernel_time_t		time_t;
#endif

#ifndef _CLOCK_T
#define _CLOCK_T
typedef __kernel_clock_t	clock_t;
#endif

#ifndef _CADDR_T
#define _CADDR_T
typedef __kernel_caddr_t	caddr_t;
#endif

/* bsd */
typedef unsigned char		u_char;
typedef unsigned short		u_short;
typedef unsigned int		u_int;
typedef unsigned long		u_long;

/* sysv */
typedef unsigned char		unchar;
typedef unsigned short		ushort;
typedef unsigned int		uint;
typedef unsigned long		ulong;

#ifndef __BIT_TYPES_DEFINED__
#define __BIT_TYPES_DEFINED__

typedef		__u8		u_int8_t;
typedef		__s8		int8_t;
typedef		__u16		u_int16_t;
typedef		__s16		int16_t;
typedef		__u32		u_int32_t;
typedef		__s32		int32_t;

#endif /* !(__BIT_TYPES_DEFINED__) */

typedef		__u8		uint8_t;
typedef		__u16		uint16_t;
typedef		__u32		uint32_t;

#if defined(__GNUC__)
typedef		__u64		uint64_t;
typedef		__u64		u_int64_t;
typedef		__s64		int64_t;
#endif

/* this is a special 64bit data type that is 8-byte aligned */
#define aligned_u64 __u64 __attribute__((aligned(8)))
#define aligned_be64 __be64 __attribute__((aligned(8)))
#define aligned_le64 __le64 __attribute__((aligned(8)))

/**
 * The type used for indexing onto a disc or disc partition.
 *
 * Linux always considers sectors to be 512 bytes long independently
 * of the devices real block size.
 *
 * blkcnt_t is the type of the inode's block count.
 */
#ifdef CONFIG_LBDAF // CONFIG_LBDAF=y
// ARM10C 20151003
typedef u64 sector_t;
// ARM10C 20151003
typedef u64 blkcnt_t;
#else
typedef unsigned long sector_t;
typedef unsigned long blkcnt_t;
#endif

/*
 * The type of an index into the pagecache.  Use a #define so asm/types.h
 * can override it.
 */
#ifndef pgoff_t
// ARM10C 20151003
#define pgoff_t unsigned long
#endif

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
typedef u64 dma_addr_t;
#else
typedef u32 dma_addr_t;
#endif /* dma_addr_t */

#ifdef __CHECKER__
#else
#endif
#ifdef __CHECK_ENDIAN__
#else
#endif
// ARM10C 20140426
typedef unsigned __bitwise__ gfp_t;
// ARM10C 20151003
// ARM10C 20151114
typedef unsigned __bitwise__ fmode_t;
// ARM10C 20150919
typedef unsigned __bitwise__ oom_flags_t;

#ifdef CONFIG_PHYS_ADDR_T_64BIT
typedef u64 phys_addr_t;
#else
typedef u32 phys_addr_t;
#endif

// ARM10C 20140125
typedef phys_addr_t resource_size_t;

/*
 * This type is the placeholder for a hardware interrupt number. It has to be
 * big enough to enclose whatever representation is used by a given platform.
 */
// ARM10C 20141108
// ARM10C 20141213
// ARM10C 20150321
typedef unsigned long irq_hw_number_t;

// ARM10C 20140329
// ARM10C 20140419
// ARM10C 20150718
// ARM10C 20150808
// ARM10C 20150919
// ARM10C 20151003
// ARM10C 20151114
// ARM10C 20151121
// ARM10C 20160319
// ARM10C 20160402
// ARM10C 20160409
// ARM10C 20160604
// ARM10C 20160716
// ARM10C 20160827
typedef struct {
	int counter;
} atomic_t;

#ifdef CONFIG_64BIT
typedef struct {
	long counter;
} atomic64_t;
#endif

// ARM10C 20131123
// ARM10C 20131207
// ARM10C 20150711
// ARM10C 20150718
// ARM10C 20150919
// ARM10C 20151003
// ARM10C 20151024
// ARM10C 20151114
// ARM10C 20160123
// ARM10C 20160409
// ARM10C 20160521
// ARM10C 20170427
// ARM10C 20170830
// sizeof(struct list_head) : 8 bytes
struct list_head {
	struct list_head *next, *prev;
};

// ARM10C 20140322
// ARM10C 20151024
// ARM10C 20151121
// ARM10C 20160730
// ARM10C 20161210
// sizeof(struct hlist_head): 4 bytes
struct hlist_head {
	struct hlist_node *first;
};

// ARM10C 20140322
// ARM10C 20150117
// ARM10C 20150808
// ARM10C 20150912
// ARM10C 20151024
// ARM10C 20151114
// ARM10C 20161203
// ARM10C 20161210
// ARM10C 20170701
// sizeof(struct hlist_node): 8 bytes
struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct ustat {
	__kernel_daddr_t	f_tfree;
	__kernel_ino_t		f_tinode;
	char			f_fname[6];
	char			f_fpack[6];
};

/**
 * struct callback_head - callback structure for use with RCU and task_work
 * @next: next update requests in a list
 * @func: actual update function to call after the grace period.
 */
// ARM10C 20140419
// ARM10C 20140809
// ARM10C 20140830
// ARM10C 20140920
// sizeof(struct callback_head): 8 bytes
struct callback_head {
	struct callback_head *next;
	void (*func)(struct callback_head *head);
};
// ARM10C 20140419
// ARM10C 20140809
// ARM10C 20140830
// ARM10C 20140920
// ARM10C 20150912
// ARM10C 20150919
// ARM10C 20151024
// ARM10C 20151114
// ARM10C 20160625
// ARM10C 20160716
// sizeof(rcu_head): 8 bytes
#define rcu_head callback_head

#endif /*  __ASSEMBLY__ */
#endif /* _LINUX_TYPES_H */
