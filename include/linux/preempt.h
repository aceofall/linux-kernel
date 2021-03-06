#ifndef __LINUX_PREEMPT_H
#define __LINUX_PREEMPT_H

/*
 * include/linux/preempt.h - macros for accessing and manipulating
 * preempt_count (used for kernel preemption, interrupt count, etc.)
 */

#include <linux/linkage.h>
#include <linux/list.h>

/*
 * We use the MSB mostly because its available; see <linux/preempt_mask.h> for
 * the other bits -- can't include that header due to inclusion hell.
 */
// ARM10C 20161029
// PREEMPT_NEED_RESCHED: 0x80000000
#define PREEMPT_NEED_RESCHED	0x80000000

#include <asm/preempt.h>

// CONFIG_DEBUG_PREEMPT=y, CONFIG_PREEMPT_TRACER=n
#if defined(CONFIG_DEBUG_PREEMPT) || defined(CONFIG_PREEMPT_TRACER)
// ARM10C 20140315
// ARM10C 20140920
// ARM10C 20141227
extern void preempt_count_add(int val);
// ARM10C 20160402
extern void preempt_count_sub(int val);
// ARM10C 20140614
// ARM10C 20160402
// ARM10C 20161029
#define preempt_count_dec_and_test() ({ preempt_count_sub(1); should_resched(); })
#else
#define preempt_count_add(val)	__preempt_count_add(val)
#define preempt_count_sub(val)	__preempt_count_sub(val)
#define preempt_count_dec_and_test() __preempt_count_dec_and_test()
#endif

#define __preempt_count_inc() __preempt_count_add(1)
#define __preempt_count_dec() __preempt_count_sub(1)

// ARM10C 20140125
// ARM10C 20140315
// ARM10C 20140920
// ARM10C 20160326
// ARM10C 20160514
// ARM10C 20161203
#define preempt_count_inc() preempt_count_add(1)
// ARM10C 20140125
// ARM10C 20170715
// ARM10C 20170830
#define preempt_count_dec() preempt_count_sub(1)

#ifdef CONFIG_PREEMPT_COUNT // ARM10C Y 

// ARM10C 20140125
// ARM10C 20140315
// ARM10C 20140517
// ARM10C 20140614
// ARM10C 20140621
// ARM10C 20140628
// ARM10C 20140920
// ARM10C 20141206
// ARM10C 20150411
// ARM10C 20151031
// ARM10C 20160326
// ARM10C 20160514
// ARM10C 20161008
// ARM10C 20161126
// ARM10C 20161203
// ARM10C 20170715
#define preempt_disable()/*ARM10C this*/	\
do { \
	preempt_count_inc(); \
	barrier(); \
} while (0)

// ARM10C 20140125
// ARM10C 20170715
#define sched_preempt_enable_no_resched() \
do { \
	barrier(); \
	preempt_count_dec(); \
} while (0)

// ARM10C 20140125
#define preempt_enable_no_resched() sched_preempt_enable_no_resched()

#ifdef CONFIG_PREEMPT // CONFIG_PREEMPT=y
// ARM10C 20140125
// ARM10C 20140322
// ARM10C 20140412
// ARM10C 20140614
// ARM10C 20140621
// ARM10C 20140628
// ARM10C 20141206
// ARM10C 20160402
// ARM10C 20161029
// ARM10C 20161210
#define preempt_enable() \
do { \
	barrier(); \
	if (unlikely(preempt_count_dec_and_test())) \
		__preempt_schedule(); \
} while (0)

// ARM10C 20130322
// TIF_NEED_RESCHED: 1
#define preempt_check_resched() \
do { \
	if (should_resched()) \
		__preempt_schedule(); \
} while (0)

#else
#define preempt_enable() preempt_enable_no_resched()
#define preempt_check_resched() do { } while (0)
#endif

// ARM10C 20130831
#define preempt_disable_notrace() \
do { \
	__preempt_count_inc(); \
	barrier(); \
} while (0)

#define preempt_enable_no_resched_notrace() \
do { \
	barrier(); \
	__preempt_count_dec(); \
} while (0)

#ifdef CONFIG_PREEMPT

#ifndef CONFIG_CONTEXT_TRACKING
#define __preempt_schedule_context() __preempt_schedule()
#endif

#define preempt_enable_notrace() \
do { \
	barrier(); \
	if (unlikely(__preempt_count_dec_and_test())) \
		__preempt_schedule_context(); \
} while (0)
#else
#define preempt_enable_notrace() preempt_enable_no_resched_notrace()
#endif

#else /* !CONFIG_PREEMPT_COUNT */

/*
 * Even if we don't have any preemption, we need preempt disable/enable
 * to be barriers, so that we don't have things like get_user/put_user
 * that can cause faults and scheduling migrate into our preempt-protected
 * region.
 */
#define preempt_disable()			barrier()
#define sched_preempt_enable_no_resched()	barrier()
#define preempt_enable_no_resched()		barrier()
#define preempt_enable()			barrier()
#define preempt_check_resched()			do { } while (0)

#define preempt_disable_notrace()		barrier()
#define preempt_enable_no_resched_notrace()	barrier()
#define preempt_enable_notrace()		barrier()

#endif /* CONFIG_PREEMPT_COUNT */

#ifdef CONFIG_PREEMPT_NOTIFIERS

struct preempt_notifier;

/**
 * preempt_ops - notifiers called when a task is preempted and rescheduled
 * @sched_in: we're about to be rescheduled:
 *    notifier: struct preempt_notifier for the task being scheduled
 *    cpu:  cpu we're scheduled on
 * @sched_out: we've just been preempted
 *    notifier: struct preempt_notifier for the task being preempted
 *    next: the task that's kicking us out
 *
 * Please note that sched_in and out are called under different
 * contexts.  sched_out is called with rq lock held and irq disabled
 * while sched_in is called without rq lock and irq enabled.  This
 * difference is intentional and depended upon by its users.
 */
struct preempt_ops {
	void (*sched_in)(struct preempt_notifier *notifier, int cpu);
	void (*sched_out)(struct preempt_notifier *notifier,
			  struct task_struct *next);
};

/**
 * preempt_notifier - key for installing preemption notifiers
 * @link: internal use
 * @ops: defines the notifier functions to be called
 *
 * Usually used in conjunction with container_of().
 */
struct preempt_notifier {
	struct hlist_node link;
	struct preempt_ops *ops;
};

void preempt_notifier_register(struct preempt_notifier *notifier);
void preempt_notifier_unregister(struct preempt_notifier *notifier);

static inline void preempt_notifier_init(struct preempt_notifier *notifier,
				     struct preempt_ops *ops)
{
	INIT_HLIST_NODE(&notifier->link);
	notifier->ops = ops;
}

#endif

#endif /* __LINUX_PREEMPT_H */
