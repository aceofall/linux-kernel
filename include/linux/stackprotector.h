#ifndef _LINUX_STACKPROTECTOR_H
#define _LINUX_STACKPROTECTOR_H 1

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/random.h>

#ifdef CONFIG_CC_STACKPROTECTOR // CONFIG_CC_STACKPROTECTOR=n
# include <asm/stackprotector.h>
#else
// KID 20140113
static inline void boot_init_stack_canary(void)
{
}
#endif

#endif
