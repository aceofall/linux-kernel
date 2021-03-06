/*  linux/include/linux/clockchips.h
 *
 *  This file contains the structure definitions for clockchips.
 *
 *  If you are not a clockchip, or the time of day code, you should
 *  not be including this file!
 */
#ifndef _LINUX_CLOCKCHIPS_H
#define _LINUX_CLOCKCHIPS_H

/* Clock event notification values */
enum clock_event_nofitiers {
	CLOCK_EVT_NOTIFY_ADD,
	CLOCK_EVT_NOTIFY_BROADCAST_ON,
	CLOCK_EVT_NOTIFY_BROADCAST_OFF,
	CLOCK_EVT_NOTIFY_BROADCAST_FORCE,
	CLOCK_EVT_NOTIFY_BROADCAST_ENTER,
	CLOCK_EVT_NOTIFY_BROADCAST_EXIT,
	CLOCK_EVT_NOTIFY_SUSPEND,
	CLOCK_EVT_NOTIFY_RESUME,
	CLOCK_EVT_NOTIFY_CPU_DYING,
	CLOCK_EVT_NOTIFY_CPU_DEAD,
};

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BUILD

#include <linux/clocksource.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/notifier.h>

struct clock_event_device;
struct module;

/* Clock event mode commands */
// ARM10C 20150411
// ARM10C 20150418
// ARM10C 20150509
// ARM10C 20150523
// ARM10C 20150620
enum clock_event_mode {
	// CLOCK_EVT_MODE_UNUSED: 0
	CLOCK_EVT_MODE_UNUSED = 0,
	// CLOCK_EVT_MODE_SHUTDOWN: 1
	CLOCK_EVT_MODE_SHUTDOWN,
	// CLOCK_EVT_MODE_PERIODIC: 2
	CLOCK_EVT_MODE_PERIODIC,
	// CLOCK_EVT_MODE_ONESHOT: 3
	CLOCK_EVT_MODE_ONESHOT,
	// CLOCK_EVT_MODE_RESUME: 4
	CLOCK_EVT_MODE_RESUME,
};

/*
 * Clock event features
 */
// ARM10C 20150404
// ARM10C 20150523
// CLOCK_EVT_FEAT_PERIODIC: 0x000001
#define CLOCK_EVT_FEAT_PERIODIC		0x000001
// ARM10C 20150404
// ARM10C 20150411
// ARM10C 20150509
// ARM10C 20150523
// CLOCK_EVT_FEAT_ONESHOT: 0x000002
#define CLOCK_EVT_FEAT_ONESHOT		0x000002
// ARM10C 20150620
// CLOCK_EVT_FEAT_KTIME: 0x000004
#define CLOCK_EVT_FEAT_KTIME		0x000004
/*
 * x86(64) specific misfeatures:
 *
 * - Clockevent source stops in C3 State and needs broadcast support.
 * - Local APIC timer is used as a dummy device.
 */
// ARM10C 20150418
// ARM10C 20150523
// CLOCK_EVT_FEAT_C3STOP: 0x000008
#define CLOCK_EVT_FEAT_C3STOP		0x000008
// ARM10C 20150418
// ARM10C 20150523
// CLOCK_EVT_FEAT_DUMMY: 0x000010
#define CLOCK_EVT_FEAT_DUMMY		0x000010

/*
 * Core shall set the interrupt affinity dynamically in broadcast mode
 */
// ARM10C 20150523
// CLOCK_EVT_FEAT_DYNIRQ: 0x000020
#define CLOCK_EVT_FEAT_DYNIRQ		0x000020
#define CLOCK_EVT_FEAT_PERCPU		0x000040

/**
 * struct clock_event_device - clock event device descriptor
 * @event_handler:	Assigned by the framework to be called by the low
 *			level handler of the event source
 * @set_next_event:	set next event function using a clocksource delta
 * @set_next_ktime:	set next event function using a direct ktime value
 * @next_event:		local storage for the next event in oneshot mode
 * @max_delta_ns:	maximum delta value in ns
 * @min_delta_ns:	minimum delta value in ns
 * @mult:		nanosecond to cycles multiplier
 * @shift:		nanoseconds to cycles divisor (power of two)
 * @mode:		operating mode assigned by the management code
 * @features:		features
 * @retries:		number of forced programming retries
 * @set_mode:		set mode function
 * @broadcast:		function to broadcast events
 * @min_delta_ticks:	minimum delta value in ticks stored for reconfiguration
 * @max_delta_ticks:	maximum delta value in ticks stored for reconfiguration
 * @name:		ptr to clock event name
 * @rating:		variable to rate clock event devices
 * @irq:		IRQ number (only for non CPU local devices)
 * @cpumask:		cpumask to indicate for which CPUs this device works
 * @list:		list head for the management code
 * @owner:		module reference
 */
// ARM10C 20150321
// ARM10C 20150411
// ARM10C 20150418
// ARM10C 20150509
// ARM10C 20150523
// ARM10C 20150620
struct clock_event_device {
	void			(*event_handler)(struct clock_event_device *);
	int			(*set_next_event)(unsigned long evt,
						  struct clock_event_device *);
	int			(*set_next_ktime)(ktime_t expires,
						  struct clock_event_device *);
	ktime_t			next_event;
	u64			max_delta_ns;
	u64			min_delta_ns;
	u32			mult;
	u32			shift;
	enum clock_event_mode	mode;
	unsigned int		features;
	unsigned long		retries;

	void			(*broadcast)(const struct cpumask *mask);
	void			(*set_mode)(enum clock_event_mode mode,
					    struct clock_event_device *);
	void			(*suspend)(struct clock_event_device *);
	void			(*resume)(struct clock_event_device *);
	unsigned long		min_delta_ticks;
	unsigned long		max_delta_ticks;

	const char		*name;
	int			rating;
	int			irq;
	const struct cpumask	*cpumask;
	struct list_head	list;
	struct module		*owner;
} ____cacheline_aligned;

/*
 * Calculate a multiplication factor for scaled math, which is used to convert
 * nanoseconds based values to clock ticks:
 *
 * clock_ticks = (nanoseconds * factor) >> shift.
 *
 * div_sc is the rearranged equation to calculate a factor from a given clock
 * ticks / nanoseconds ratio:
 *
 * factor = (clock_ticks << shift) / nanoseconds
 */
static inline unsigned long div_sc(unsigned long ticks, unsigned long nsec,
				   int shift)
{
	uint64_t tmp = ((uint64_t)ticks) << shift;

	do_div(tmp, nsec);
	return (unsigned long) tmp;
}

/* Clock event layer functions */
extern u64 clockevent_delta2ns(unsigned long latch,
			       struct clock_event_device *evt);
extern void clockevents_register_device(struct clock_event_device *dev);
extern int clockevents_unbind_device(struct clock_event_device *ced, int cpu);

extern void clockevents_config(struct clock_event_device *dev, u32 freq);
extern void clockevents_config_and_register(struct clock_event_device *dev,
					    u32 freq, unsigned long min_delta,
					    unsigned long max_delta);

extern int clockevents_update_freq(struct clock_event_device *ce, u32 freq);

extern void clockevents_exchange_device(struct clock_event_device *old,
					struct clock_event_device *new);
extern void clockevents_set_mode(struct clock_event_device *dev,
				 enum clock_event_mode mode);
extern int clockevents_program_event(struct clock_event_device *dev,
				     ktime_t expires, bool force);

extern void clockevents_handle_noop(struct clock_event_device *dev);

// ARM10C 20150411
// dev: [pcp0] &(&percpu_mct_tick)->evt, freq: 12000000, sec: 178
// ARM10C 20150523
// dev: &mct_comp_device, freq: 24000000, sec: 178
static inline void
clockevents_calc_mult_shift(struct clock_event_device *ce, u32 freq, u32 minsec)
{
	// &ce->mult: [pcp0] &(&(&percpu_mct_tick)->evt)->mult,
	// &ce->shift: [pcp0] &(&(&percpu_mct_tick)->evt)->shift,
	// NSEC_PER_SEC: 1000000000L, freq: 12000000, minsec: 178
	// clocks_calc_mult_shift([pcp0] &(&(&percpu_mct_tick)->evt)->mult,
	// [pcp0] &(&(&percpu_mct_tick)->evt)->shift, 1000000000L, 12000000, 178)
	// &ce->mult: &(&mct_comp_device)->mult, &ce->shift: &(&mct_comp_device)->shift,
	// NSEC_PER_SEC: 1000000000L, freq: 24000000, minsec: 178
	// clocks_calc_mult_shift(&(&mct_comp_device)->mult, &(&mct_comp_device)->shift, 1000000000L, 24000000, 178)
	return clocks_calc_mult_shift(&ce->mult, &ce->shift, NSEC_PER_SEC,
				      freq, minsec);

	// clocks_calc_mult_shift에서 한일:
	// *mult: [pcp0] (&(&percpu_mct_tick)->evt)->mult: 0x3126E98
	// *shift: [pcp0] (&(&percpu_mct_tick)->evt)->shift: 32

	// clocks_calc_mult_shift에서 한일:
	// *mult: (&mct_comp_device)->mult: 0x3126E98
	// *shift: (&mct_comp_device)->shift: 31
}

extern void clockevents_suspend(void);
extern void clockevents_resume(void);

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
#ifdef CONFIG_ARCH_HAS_TICK_BROADCAST
extern void tick_broadcast(const struct cpumask *mask);
#else
#define tick_broadcast	NULL
#endif
extern int tick_receive_broadcast(void);
#endif

#if defined(CONFIG_GENERIC_CLOCKEVENTS_BROADCAST) && defined(CONFIG_TICK_ONESHOT)
extern int tick_check_broadcast_expired(void);
#else
static inline int tick_check_broadcast_expired(void) { return 0; }
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS
extern void clockevents_notify(unsigned long reason, void *arg);
#else
static inline void clockevents_notify(unsigned long reason, void *arg) {}
#endif

#else /* CONFIG_GENERIC_CLOCKEVENTS_BUILD */

static inline void clockevents_suspend(void) {}
static inline void clockevents_resume(void) {}

static inline void clockevents_notify(unsigned long reason, void *arg) {}
static inline int tick_check_broadcast_expired(void) { return 0; }

#endif

#endif
