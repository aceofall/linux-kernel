#ifndef _LINUX_TIMERQUEUE_H
#define _LINUX_TIMERQUEUE_H

#include <linux/rbtree.h>
#include <linux/ktime.h>


// ARM10C 20140830
// sizeof(struct timerqueue_node): 20 bytes
struct timerqueue_node {
	struct rb_node node;
	ktime_t expires;
};

struct timerqueue_head {
	struct rb_root head;
	struct timerqueue_node *next;
};


extern void timerqueue_add(struct timerqueue_head *head,
				struct timerqueue_node *node);
extern void timerqueue_del(struct timerqueue_head *head,
				struct timerqueue_node *node);
extern struct timerqueue_node *timerqueue_iterate_next(
						struct timerqueue_node *node);

/**
 * timerqueue_getnext - Returns the timer with the earliest expiration time
 *
 * @head: head of timerqueue
 *
 * Returns a pointer to the timer node that has the
 * earliest expiration time.
 */
static inline
struct timerqueue_node *timerqueue_getnext(struct timerqueue_head *head)
{
	return head->next;
}

// ARM10C 20140830
// &timer->base: &(&(&def_rt_bandwidth)->rt_period_timer)->node
// ARM10C 20140913
// &timer->node: &(&(&runqueues)->hrtick_timer)->node
static inline void timerqueue_init(struct timerqueue_node *node)
{
	// &node->node: (&(&(&def_rt_bandwidth)->rt_period_timer)->node)->node
	// RB_CLEAR_NODE((&(&(&def_rt_bandwidth)->rt_period_timer)->node)->node):
	// (((&(&(&def_rt_bandwidth)->rt_period_timer)->node)->node)->__rb_parent_color =
	// (unsigned long)((&(&(&def_rt_bandwidth)->rt_period_timer)->node)->node))
	// &node->node: (&(&(&runqueues)->hrtick_timer)->node)->node
	// RB_CLEAR_NODE((&(&(&runqueues)->hrtick_timer)->node)->node):
	// (((&(&(&runqueues)->hrtick_timer)->node)->node)->__rb_parent_color =
	// (unsigned long)(((&(&(&runqueues)->hrtick_timer)->node)->node))
	RB_CLEAR_NODE(&node->node);
}

static inline void timerqueue_init_head(struct timerqueue_head *head)
{
	head->head = RB_ROOT;
	head->next = NULL;
}
#endif /* _LINUX_TIMERQUEUE_H */
