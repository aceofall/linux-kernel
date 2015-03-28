﻿/*
 * Procedures for creating, accessing and interpreting the device tree.
 *
 * Paul Mackerras	August 1996.
 * Copyright (C) 1996-2005 Paul Mackerras.
 *
 *  Adapted for 64bit PowerPC by Dave Engebretsen and Peter Bergner.
 *    {engebret|bergner}@us.ibm.com
 *
 *  Adapted for sparc and sparc64 by David S. Miller davem@davemloft.net
 *
 *  Reconsolidated from arch/x/kernel/prom.c by Stephen Rothwell and
 *  Grant Likely.
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
#include <linux/ctype.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>

#include "of_private.h"

// ARM10C 20140215
LIST_HEAD(aliases_lookup);

// ARM10C 20140208
// ARM10C 20141004
// ARM10C 20141011
struct device_node *of_allnodes;
EXPORT_SYMBOL(of_allnodes);
struct device_node *of_chosen;
// ARM10C 20140215
struct device_node *of_aliases;
static struct device_node *of_stdout;

DEFINE_MUTEX(of_aliases_mutex);

/* use when traversing tree through the allnext, child, sibling,
 * or parent members of struct device_node.
 */
// ARM10C 20140215
// ARM10C 20141004
// ARM10C 20150307
DEFINE_RAW_SPINLOCK(devtree_lock);

// ARM10C 20141018
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
// ARM10C 20141101
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
int of_n_addr_cells(struct device_node *np)
{
	const __be32 *ip;

	do {
		// np->parent: (gic node의 주소)->parent: root node의 주소
		// np->parent: (gic node의 주소)->parent: root node의 주소
		if (np->parent)
			// np->parent: (gic node의 주소)->parent: root node의 주소
			// np->parent: (gic node의 주소)->parent: root node의 주소
			np = np->parent;
			// np: root node의 주소
			// np: root node의 주소

		// np: root node의 주소,
		// of_get_property(root node의 주소, "#address-cells", NULL): 1
		// np: root node의 주소,
		// of_get_property(root node의 주소, "#address-cells", NULL): 1
		ip = of_get_property(np, "#address-cells", NULL);
		// ip: 1
		// ip: 1

		// ip: 1
		// ip: 1
		if (ip)
			// ip: 1
			// ip: 1
			return be32_to_cpup(ip);
			// return 1
			// return 1

	} while (np->parent);
	/* No #address-cells property for the root node */
	return OF_ROOT_NODE_ADDR_CELLS_DEFAULT;
}
EXPORT_SYMBOL(of_n_addr_cells);

// ARM10C 20141018
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
// ARM10C 20141101
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
int of_n_size_cells(struct device_node *np)
{
	const __be32 *ip;

	do {
		// np->parent: (gic node의 주소)->parent: root node의 주소
		// np->parent: (gic node의 주소)->parent: root node의 주소
		if (np->parent)
			// np->parent: (gic node의 주소)->parent: root node의 주소
			// np->parent: (gic node의 주소)->parent: root node의 주소
			np = np->parent;
			// np: root node의 주소
			// np: root node의 주소

		// np: root node의 주소,
		// of_get_property(root node의 주소, "#size-cells", NULL): 1
		// np: root node의 주소,
		// of_get_property(root node의 주소, "#size-cells", NULL): 1
		ip = of_get_property(np, "#size-cells", NULL);
		// ip: 1
		// ip: 1

		// ip: 1
		// ip: 1
		if (ip)
			// ip: 1
			// ip: 1
			return be32_to_cpup(ip);
			// return 1
			// return 1

	} while (np->parent);
	/* No #size-cells property for the root node */
	return OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
}
EXPORT_SYMBOL(of_n_size_cells);

#ifdef CONFIG_NUMA
int __weak of_node_to_nid(struct device_node *np)
{
	return numa_node_id();
}
#endif

#if defined(CONFIG_OF_DYNAMIC) // CONFIG_OF_DYNAMIC=n
/**
 *	of_node_get - Increment refcount of a node
 *	@node:	Node to inc refcount, NULL is supported to
 *		simplify writing of callers
 *
 *	Returns node.
 */
struct device_node *of_node_get(struct device_node *node)
{
	if (node)
		kref_get(&node->kref);
	return node;
}
EXPORT_SYMBOL(of_node_get);

static inline struct device_node *kref_to_device_node(struct kref *kref)
{
	return container_of(kref, struct device_node, kref);
}

/**
 *	of_node_release - release a dynamically allocated node
 *	@kref:  kref element of the node to be released
 *
 *	In of_node_put() this function is passed to kref_put()
 *	as the destructor.
 */
static void of_node_release(struct kref *kref)
{
	struct device_node *node = kref_to_device_node(kref);
	struct property *prop = node->properties;

	/* We should never be releasing nodes that haven't been detached. */
	if (!of_node_check_flag(node, OF_DETACHED)) {
		pr_err("ERROR: Bad of_node_put() on %s\n", node->full_name);
		dump_stack();
		kref_init(&node->kref);
		return;
	}

	if (!of_node_check_flag(node, OF_DYNAMIC))
		return;

	while (prop) {
		struct property *next = prop->next;
		kfree(prop->name);
		kfree(prop->value);
		kfree(prop);
		prop = next;

		if (!prop) {
			prop = node->deadprops;
			node->deadprops = NULL;
		}
	}
	kfree(node->full_name);
	kfree(node->data);
	kfree(node);
}

/**
 *	of_node_put - Decrement refcount of a node
 *	@node:	Node to dec refcount, NULL is supported to
 *		simplify writing of callers
 *
 */
void of_node_put(struct device_node *node)
{
	if (node)
		kref_put(&node->kref, of_node_release);
}
EXPORT_SYMBOL(of_node_put);
#endif /* CONFIG_OF_DYNAMIC */

// ARM10C 20140208
// ARM10C 20141004
// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소, name: "interrupt-controller", lenp: NULL
static struct property *__of_find_property(const struct device_node *np,
					   const char *name, int *lenp)
{
	struct property *pp;

	// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	if (!np)
		return NULL;

	// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	for (pp = np->properties; pp; pp = pp->next) {
		// pp: pp->name이 "interrupt-controller" 일때의 주소 (exynos5.dtsi)

		// pp->name: "interrupt-controller", name: "interrupt-controller"
		// of_prop_cmp("interrupt-controller", "interrupt-controller"): 0
		if (of_prop_cmp(pp->name, name) == 0) {
			// lenp: NULL
			if (lenp)
				*lenp = pp->length;
			break;
		}
	}

	// pp: combiner node의 "interrupt-controller" property의 주소
	return pp;
	// return combiner node의 "interrupt-controller" property의 주소
}

// ARM10C 20140208
// ARM10C 20140215
// np: cpu0의 node의 주소값, propname: "reg", NULL
// ARM10C 20141004
// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소, "interrupt-controller", NULL
// ARM10C 20141011
// np: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소 (cortex_a15_gic), "interrupt-controller", NULL
// ARM10C 20141018
// np: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소, propname: "reg-names"
// ARM10C 20150321
// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, propname: "clock-names"
struct property *of_find_property(const struct device_node *np,
				  const char *name,
				  int *lenp)
{
	struct property *pp;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장

	// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소, name: "interrupt-controller", lenp: NULL
	// __of_find_property(devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소, "interrupt-controller", NULL):
	// combiner node의 "interrupt-controller" property의 주소
	pp = __of_find_property(np, name, lenp);
	// pp: combiner node의 "interrupt-controller" property의 주소

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원

	// pp: combiner node의 "interrupt-controller" property의 주소
	return pp;
	// return: combiner node의 "interrupt-controller" property의 주소
}
EXPORT_SYMBOL(of_find_property);

/**
 * of_find_all_nodes - Get next node in global list
 * @prev:	Previous node or NULL to start iteration
 *		of_node_put() will be called on it
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
struct device_node *of_find_all_nodes(struct device_node *prev)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = prev ? prev->allnext : of_allnodes;
	for (; np != NULL; np = np->allnext)
		if (of_node_get(np))
			break;
	of_node_put(prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_all_nodes);

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
// ARM10C 20141004
// ARM10C 20150307
static const void *__of_get_property(const struct device_node *np,
				     const char *name, int *lenp)
{
	struct property *pp = __of_find_property(np, name, lenp);

	return pp ? pp->value : NULL;
}

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
// ARM10C 20140208
// ARM10C 20141011
// ARM10C 20141018
// ARM10C 20141101
// ARM10C 20141213
// ARM10C 20150307
// ARM10C 20150314
// ARM10C 20150321
const void *of_get_property(const struct device_node *np, const char *name,
			    int *lenp)
{
	struct property *pp = of_find_property(np, name, lenp);

	return pp ? pp->value : NULL;
}
EXPORT_SYMBOL(of_get_property);

/*
 * arch_match_cpu_phys_id - Match the given logical CPU and physical id
 *
 * @cpu: logical cpu index of a core/thread
 * @phys_id: physical identifier of a core/thread
 *
 * CPU logical to physical index mapping is architecture specific.
 * However this __weak function provides a default match of physical
 * id to logical cpu index. phys_id provided here is usually values read
 * from the device tree which must match the hardware internal registers.
 *
 * Returns true if the physical identifier and the logical cpu index
 * correspond to the same core/thread, false otherwise.
 */
bool __weak arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return (u32)phys_id == cpu;
}

/**
 * Checks if the given "prop_name" property holds the physical id of the
 * core/thread corresponding to the logical cpu 'cpu'. If 'thread' is not
 * NULL, local thread number within the core is returned in it.
 */
static bool __of_find_n_match_cpu_property(struct device_node *cpun,
			const char *prop_name, int cpu, unsigned int *thread)
{
	const __be32 *cell;
	int ac, prop_len, tid;
	u64 hwid;

	ac = of_n_addr_cells(cpun);
	cell = of_get_property(cpun, prop_name, &prop_len);
	if (!cell || !ac)
		return false;
	prop_len /= sizeof(*cell) * ac;
	for (tid = 0; tid < prop_len; tid++) {
		hwid = of_read_number(cell, ac);
		if (arch_match_cpu_phys_id(cpu, hwid)) {
			if (thread)
				*thread = tid;
			return true;
		}
		cell += ac;
	}
	return false;
}

/*
 * arch_find_n_match_cpu_physical_id - See if the given device node is
 * for the cpu corresponding to logical cpu 'cpu'.  Return true if so,
 * else false.  If 'thread' is non-NULL, the local thread number within the
 * core is returned in it.
 */
bool __weak arch_find_n_match_cpu_physical_id(struct device_node *cpun,
					      int cpu, unsigned int *thread)
{
	/* Check for non-standard "ibm,ppc-interrupt-server#s" property
	 * for thread ids on PowerPC. If it doesn't exist fallback to
	 * standard "reg" property.
	 */
	if (IS_ENABLED(CONFIG_PPC) &&
	    __of_find_n_match_cpu_property(cpun,
					   "ibm,ppc-interrupt-server#s",
					   cpu, thread))
		return true;

	if (__of_find_n_match_cpu_property(cpun, "reg", cpu, thread))
		return true;

	return false;
}

/**
 * of_get_cpu_node - Get device node associated with the given logical CPU
 *
 * @cpu: CPU number(logical index) for which device node is required
 * @thread: if not NULL, local thread number within the physical core is
 *          returned
 *
 * The main purpose of this function is to retrieve the device node for the
 * given logical CPU index. It should be used to initialize the of_node in
 * cpu device. Once of_node in cpu device is populated, all the further
 * references can use that instead.
 *
 * CPU logical to physical index mapping is architecture specific and is built
 * before booting secondary cores. This function uses arch_match_cpu_phys_id
 * which can be overridden by architecture specific implementation.
 *
 * Returns a node pointer for the logical cpu if found, else NULL.
 */
struct device_node *of_get_cpu_node(int cpu, unsigned int *thread)
{
	struct device_node *cpun;

	for_each_node_by_type(cpun, "cpu") {
		if (arch_find_n_match_cpu_physical_id(cpun, cpu, thread))
			return cpun;
	}
	return NULL;
}
EXPORT_SYMBOL(of_get_cpu_node);

/** Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
// ARM10C 20141004
// node: of_allnodes,
// matches->compatible: irqchip_of_match_exynos4210_combiner->compatible: "samsung,exynos4210-combiner"
// ARM10C 20141011
// node: gic node의 주소
// matches->compatible: irqchip_of_match_cortex_a15_gic->compatible: "arm,cortex-a15-gic"
static int __of_device_is_compatible(const struct device_node *device,
				     const char *compat)
{
	const char* cp;
	int cplen, l;
	
	// exynos5.dtsi 에 정의된
	// compatible 이 "samsung,exynos4210-combiner" 인 combiner node를 찾음
	// exynos5.dtsi 에 정의된
	// compatible 이 "arm,cortex-a15-gic" 인 gic node를 찾음

	// device: of_allnodes
	// device: gic node의 주소
	cp = __of_get_property(device, "compatible", &cplen);
	// cp: "samsung,exynos4210-combiner", cplen: 28
	// cp: "arm,cortex-a15-gic""arm,cortex-a9-gic", cplen: 37

	// cp: "samsung,exynos4210-combiner"
	// cp: "arm,cortex-a15-gic""arm,cortex-a9-gic"
	if (cp == NULL)
		return 0;

	// cplen: 28
	// cplen: 37
	while (cplen > 0) {
		// cp: "samsung,exynos4210-combiner", compat: "samsung,exynos4210-combiner"
		// of_compat_cmp("samsung,exynos4210-combiner", "samsung,exynos4210-combiner", 28): 0
		// cp: "arm,cortex-a15-gic""arm,cortex-a9-gic", compat: "arm,cortex-a15-gic"
		// of_compat_cmp("arm,cortex-a15-gic""arm,cortex-a9-gic", "arm,cortex-a15-gic", 19): 0
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return 1;
			// return 1
			// return 1

		l = strlen(cp) + 1;

		cp += l;

		cplen -= l;
	}

	return 0;
}

/** Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
int of_device_is_compatible(const struct device_node *device,
		const char *compat)
{
	unsigned long flags;
	int res;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	res = __of_device_is_compatible(device, compat);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return res;
}
EXPORT_SYMBOL(of_device_is_compatible);

/**
 * of_machine_is_compatible - Test root of device tree for a given compatible value
 * @compat: compatible string to look for in root node's compatible property.
 *
 * Returns true if the root node has the given value in its
 * compatible property.
 */
int of_machine_is_compatible(const char *compat)
{
	struct device_node *root;
	int rc = 0;

	root = of_find_node_by_path("/");
	if (root) {
		rc = of_device_is_compatible(root, compat);
		of_node_put(root);
	}
	return rc;
}
EXPORT_SYMBOL(of_machine_is_compatible);

/**
 *  __of_device_is_available - check if a device is available for use
 *
 *  @device: Node to check for availability, with locks already held
 *
 *  Returns 1 if the status property is absent or set to "okay" or "ok",
 *  0 otherwise
 */
// ARM10C 20150307
// device: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소
static int __of_device_is_available(const struct device_node *device)
{
	const char *status;
	int statlen;

	// device: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소
	// __of_get_property(devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, "status", &statlen): NULL
	status = __of_get_property(device, "status", &statlen);
	// status: NULL

	// status: NULL
	if (status == NULL)
		return 1;
		// return 1

	if (statlen > 0) {
		if (!strcmp(status, "okay") || !strcmp(status, "ok"))
			return 1;
	}

	return 0;
}

/**
 *  of_device_is_available - check if a device is available for use
 *
 *  @device: Node to check for availability
 *
 *  Returns 1 if the status property is absent or set to "okay" or "ok",
 *  0 otherwise
 */
// ARM10C 20150307
// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소
int of_device_is_available(const struct device_node *device)
{
	unsigned long flags;
	int res;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장

	// device: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소
	// __of_device_is_available(__clksrc_of_table_exynos4210): 1
	res = __of_device_is_available(device);
	// res: 1

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원

	// res: 1
	return res;
	// return 1

}
EXPORT_SYMBOL(of_device_is_available);

/**
 *	of_get_parent - Get a node's parent if any
 *	@node:	Node to get parent
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
// ARM10C 20141004
// ARM10C 20141011
// child: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
// ARM10C 20141018
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
struct device_node *of_get_parent(const struct device_node *node)
{
	struct device_node *np;
	unsigned long flags;

	// node: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장

	// node->parent: devtree에서 allnext로 순회 하면서 찾은 combiner node의 parent주소
	np = of_node_get(node->parent);
	// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 parent주소

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원

	// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 parent주소
	return np;
	// return devtree에서 allnext로 순회 하면서 찾은 combiner node의 parent주소
}
EXPORT_SYMBOL(of_get_parent);

/**
 *	of_get_next_parent - Iterate to a node's parent
 *	@node:	Node to get parent of
 *
 * 	This is like of_get_parent() except that it drops the
 * 	refcount on the passed node, making it suitable for iterating
 * 	through a node's parents.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_next_parent(struct device_node *node)
{
	struct device_node *parent;
	unsigned long flags;

	if (!node)
		return NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	parent = of_node_get(node->parent);
	of_node_put(node);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return parent;
}
EXPORT_SYMBOL(of_get_next_parent);

/**
 *	of_get_next_child - Iterate a node childs
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
// ARM10C 20140215
// of_get_next_child(cpus, NULL)
struct device_node *of_get_next_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
        // prev: NULL
	next = prev ? prev->sibling : node->child;
        // next: cpus->child, cpu0의주소

	for (; next; next = next->sibling)
		if (of_node_get(next))
			break;
	of_node_put(prev); // null function
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return next;
}
EXPORT_SYMBOL(of_get_next_child);

/**
 *	of_get_next_available_child - Find the next available child node
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *      This function is like of_get_next_child(), except that it
 *      automatically skips any disabled nodes (i.e. status = "disabled").
 */
struct device_node *of_get_next_available_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = prev ? prev->sibling : node->child;
	for (; next; next = next->sibling) {
		if (!__of_device_is_available(next))
			continue;
		if (of_node_get(next))
			break;
	}
	of_node_put(prev);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return next;
}
EXPORT_SYMBOL(of_get_next_available_child);

/**
 *	of_get_child_by_name - Find the child node by name for a given parent
 *	@node:	parent node
 *	@name:	child name to look for.
 *
 *      This function looks for child node for given matching name
 *
 *	Returns a node pointer if found, with refcount incremented, use
 *	of_node_put() on it when done.
 *	Returns NULL if node is not found.
 */
struct device_node *of_get_child_by_name(const struct device_node *node,
				const char *name)
{
	struct device_node *child;

	for_each_child_of_node(node, child)
		if (child->name && (of_node_cmp(child->name, name) == 0))
			break;
	return child;
}
EXPORT_SYMBOL(of_get_child_by_name);

/**
 *	of_find_node_by_path - Find a node matching a full OF path
 *	@path:	The full path to match
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
// ARM10C 20140208
struct device_node *of_find_node_by_path(const char *path)
{
	struct device_node *np = of_allnodes;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	for (; np; np = np->allnext) {
		if (np->full_name && (of_node_cmp(np->full_name, path) == 0)
		    && of_node_get(np))
			break;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_path);

/**
 *	of_find_node_by_name - Find a node by its "name" property
 *	@from:	The node to start searching from or NULL, the node
 *		you pass will not be searched, only the next one
 *		will; typically, you pass what the previous call
 *		returned. of_node_put() will be called on it
 *	@name:	The name string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_name(struct device_node *from,
	const char *name)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = from ? from->allnext : of_allnodes;
	for (; np; np = np->allnext)
		if (np->name && (of_node_cmp(np->name, name) == 0)
		    && of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_name);

/**
 *	of_find_node_by_type - Find a node by its "device_type" property
 *	@from:	The node to start searching from, or NULL to start searching
 *		the entire device tree. The node you pass will not be
 *		searched, only the next one will; typically, you pass
 *		what the previous call returned. of_node_put() will be
 *		called on from for you.
 *	@type:	The type string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_type(struct device_node *from,
	const char *type)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = from ? from->allnext : of_allnodes;
	for (; np; np = np->allnext)
		if (np->type && (of_node_cmp(np->type, type) == 0)
		    && of_node_get(np))
			break;
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_type);

/**
 *	of_find_compatible_node - Find a node based on type and one of the
 *                                tokens in its "compatible" property
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@type:		The type string to match "device_type" or NULL to ignore
 *	@compatible:	The string to match to one of the tokens in the device
 *			"compatible" list.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_compatible_node(struct device_node *from,
	const char *type, const char *compatible)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = from ? from->allnext : of_allnodes;
	for (; np; np = np->allnext) {
		if (type
		    && !(np->type && (of_node_cmp(np->type, type) == 0)))
			continue;
		if (__of_device_is_compatible(np, compatible) &&
		    of_node_get(np))
			break;
	}
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_compatible_node);

/**
 *	of_find_node_with_property - Find a node which has a property with
 *                                   the given name.
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@prop_name:	The name of the property to look for.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_with_property(struct device_node *from,
	const char *prop_name)
{
	struct device_node *np;
	struct property *pp;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np = from ? from->allnext : of_allnodes;
	for (; np; np = np->allnext) {
		for (pp = np->properties; pp; pp = pp->next) {
			if (of_prop_cmp(pp->name, prop_name) == 0) {
				of_node_get(np);
				goto out;
			}
		}
	}
out:
	of_node_put(from);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	return np;
}
EXPORT_SYMBOL(of_find_node_with_property);

// ARM10C 20141004
// matches: irqchip_of_match_exynos4210_combiner, np: of_allnodes
// ARM10C 20141011
// matches: irqchip_of_match_cortex_a15_gic, np: gic node의 주소
// ARM10C 20141011
// matches: irqchip_of_match_exynos4210_combiner, node: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
// ARM10C 20141206
// matches: irqchip_of_match_cortex_a15_gic, node: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
static
const struct of_device_id *__of_match_node(const struct of_device_id *matches,
					   const struct device_node *node)
{
	// matches: irqchip_of_match_exynos4210_combiner
	// matches: irqchip_of_match_cortex_a15_gic
	if (!matches)
		return NULL;

	// matches->name[0]: irqchip_of_match_exynos4210_combiner->name[0]: NULL,
	// matches->type[0]: irqchip_of_match_exynos4210_combiner->type[0]: NULL,
	// matches->compatible[0]: irqchip_of_match_exynos4210_combiner->compatible[0]: 's'
	// matches->name[0]: irqchip_of_match_cortex_a15_gic->name[0]: NULL,
	// matches->type[0]: irqchip_of_match_cortex_a15_gic->type[0]: NULL,
	// matches->compatible[0]: irqchip_of_match_cortex_a15_gic->compatible[0]: 'a'
	while (matches->name[0] || matches->type[0] || matches->compatible[0]) {
		int match = 1;
		// match: 1
		// match: 1

		// matches->name[0]: irqchip_of_match_exynos4210_combiner->name[0]: NULL
		// matches->name[0]: irqchip_of_match_cortex_a15_gic->name[0]: NULL
		if (matches->name[0])
			match &= node->name
				&& !strcmp(matches->name, node->name);

		// matches->type[0]: irqchip_of_match_exynos4210_combiner->type[0]: NULL
		// matches->type[0]: irqchip_of_match_cortex_a15_gic->type[0]: NULL
		if (matches->type[0])
			match &= node->type
				&& !strcmp(matches->type, node->type);

		// matches->compatible[0]: irqchip_of_match_exynos4210_combiner->compatible[0]: 's'
		// matches->compatible[0]: irqchip_of_match_cortex_a15_gic->compatible[0]: 'a'
		if (matches->compatible[0])
			// match: 1, node: of_allnodes,
			// matches->compatible: irqchip_of_match_exynos4210_combiner->compatible: "samsung,exynos4210-combiner"
			// __of_device_is_compatible(of_allnodes, "samsung,exynos4210-combiner"): 1
			// match: 1, node: gic node의 주소,
			// matches->compatible: irqchip_of_match_cortex_a15_gic->compatible: "arm,cortex-a15-gic"
			// __of_device_is_compatible(gic node의 주소, "arm,cortex-a15-gic"): 1
			match &= __of_device_is_compatible(node,
							   matches->compatible);
			// match: 1
			// match: 1

		// match: 1
		// match: 1
		if (match)
			// matches: irqchip_of_match_exynos4210_combiner
			// matches: irqchip_of_match_cortex_a15_gic
			return matches;
			// return irqchip_of_match_exynos4210_combiner
			// return irqchip_of_match_cortex_a15_gic

		matches++;
	}
	return NULL;
}

/**
 * of_match_node - Tell if an device_node has a matching of_match structure
 *	@matches:	array of of device match structures to search in
 *	@node:		the of device structure to match against
 *
 *	Low level utility function used by device matching.
 */
// ARM10C 20141011
// matches: irqchip_of_match_cortex_a15_gic,
// desc->dev: (kmem_cache#30-o11)->dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
// ARM10C 20141206
// matches: irqchip_of_match_cortex_a15_gic,
// desc->dev: (kmem_cache#30-o10)->dev: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
const struct of_device_id *of_match_node(const struct of_device_id *matches,
					 const struct device_node *node)
{
	const struct of_device_id *match;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장

	// matches: irqchip_of_match_cortex_a15_gic, node: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
	// __of_match_node(irqchip_of_match_cortex_a15_gic, devtree에서 allnext로 순회 하면서 찾은 gic node의 주소):
	// irqchip_of_match_cortex_a15_gic
	// matches: irqchip_of_match_cortex_a15_gic, node: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	// __of_match_node(irqchip_of_match_cortex_a15_gic, devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소):
	// irqchip_of_match_exynos4210_combiner
	match = __of_match_node(matches, node);
	// match: irqchip_of_match_cortex_a15_gic
	// match: irqchip_of_match_exynos4210_combiner

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원

	// match: irqchip_of_match_cortex_a15_gic
	// match: irqchip_of_match_exynos4210_combiner
	return match;
	// return irqchip_of_match_cortex_a15_gic
	// return irqchip_of_match_exynos4210_combiner
}
EXPORT_SYMBOL(of_match_node);

/**
 *	of_find_matching_node_and_match - Find a node based on an of_device_id
 *					  match table.
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@matches:	array of of device match structures to search in
 *	@match		Updated to point at the matches entry which matched
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
// ARM10C 20141004
// from: null, matches: irqchip_of_match_exynos4210_combiner, NULL
// ARM10C 20141011
// devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소,
// irqchip_of_match_exynos4210_combiner, NULL
// ARM10C 20150103
// from: NULL
// matches:
// __clk_of_table_fixed_factor_clk
// __clk_of_table_fixed_clk
// __clk_of_table_exynos4210_audss_clk
// __clk_of_table_exynos5250_audss_clk
// __clk_of_table_exynos5420_clk
// &match
// ARM10C 20150307
// from: NULL
// matches:
// __clksrc_of_table_armv7_arch_timer
// __clksrc_of_table_armv8_arch_timer
// __clksrc_of_table_armv7_arch_timer_mem
// __clksrc_of_table_exynos4210
// __clksrc_of_table_exynos4412
// &match
struct device_node *of_find_matching_node_and_match(struct device_node *from,
					const struct of_device_id *matches,
					const struct of_device_id **match)
{
	struct device_node *np;
	const struct of_device_id *m;
	unsigned long flags;

	// match: NULL
	// match: NULL
	if (match)
		*match = NULL;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장

	// from: NULL
	// from: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	np = from ? from->allnext : of_allnodes;
	// np: of_allnodes
	// np: gic node의 주소

	// unflatten_device_tree 에서 만든 tree의 root node가 of_allnodes에 저장되어 있음

	// np: of_allnodes
	// np: gic node의 주소
	for (; np; np = np->allnext) {
		// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
		// np: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소

		// matches: irqchip_of_match_exynos4210_combiner, np: of_allnodes
		// __of_match_node(irqchip_of_match_exynos4210_combiner, of_allnodes): irqchip_of_match_exynos4210_combiner
		// matches: irqchip_of_match_cortex_a15_gic, np: gic node의 주소
		// __of_match_node(irqchip_of_match_cortex_a15_gic, gic node의 주소): irqchip_of_match_cortex_a15_gic
		m = __of_match_node(matches, np);
		// m: irqchip_of_match_exynos4210_combiner
		// m: irqchip_of_match_cortex_a15_gic

		// m: irqchip_of_match_exynos4210_combiner,
		// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
		// of_node_get(np): np
		// m: irqchip_of_match_cortex_a15_gic,
		// np: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
		// of_node_get(np): np
		if (m && of_node_get(np)) {
			// match: NULL
			// match: NULL
			if (match)
				*match = m;
			break;
		}
	}

	// from: null
	// from: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	of_node_put(from); // null function

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원
	// devtree_lock을 사용한 spin lock 해재하고 flags에 저장된 cpsr을 복원

	// np: devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	// np: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
	return np;
	// return devtree에서 allnext로 순회 하면서 찾은 combiner node의 주소
	// return devtree에서 allnext로 순회 하면서 찾은 gic node의 주소
}
EXPORT_SYMBOL(of_find_matching_node_and_match);

/**
 * of_modalias_node - Lookup appropriate modalias for a device node
 * @node:	pointer to a device tree node
 * @modalias:	Pointer to buffer that modalias value will be copied into
 * @len:	Length of modalias value
 *
 * Based on the value of the compatible property, this routine will attempt
 * to choose an appropriate modalias value for a particular device tree node.
 * It does this by stripping the manufacturer prefix (as delimited by a ',')
 * from the first entry in the compatible list property.
 *
 * This routine returns 0 on success, <0 on failure.
 */
int of_modalias_node(struct device_node *node, char *modalias, int len)
{
	const char *compatible, *p;
	int cplen;

	compatible = of_get_property(node, "compatible", &cplen);
	if (!compatible || strlen(compatible) > cplen)
		return -ENODEV;
	p = strchr(compatible, ',');
	strlcpy(modalias, p ? p + 1 : compatible, len);
	return 0;
}
EXPORT_SYMBOL_GPL(of_modalias_node);

/**
 * of_find_node_by_phandle - Find a node given a phandle
 * @handle:	phandle of the node to find
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
// ARM10C 20141011
// parp: exynos5420 dtb상의 gic 의 주소
// ARM10C 20150307
// parp: exynos5420 dtb상의 mct_map 의 주소
// ARM10C 20150314
// mct_map node의 interrupt-map의 property 값의 주소+4
// ARM10C 20150321
// phandle: exynos5420 dtb상의 clock node의 주소
struct device_node *of_find_node_by_phandle(phandle handle)
{
	struct device_node *np;
	unsigned long flags;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장
	// devtree_lock을 사용한 spin lock 수행하고 cpsr을 flags에 저장

	for (np = of_allnodes; np; np = np->allnext)
		// np->phandle: exynos5420 dtb상의 gic 의 주소, handle: exynos5420 dtb상의 gic 의 주소
		// np->phandle: exynos5420 dtb상의 mct_map 의 주소, handle: exynos5420 dtb상의 mct_map 의 주소
		if (np->phandle == handle)
			break;
			// break

	// NOTE:
	// np->phandle에 값이 존재하기 위해선 exynos5420 용 dts 파일들의 node 들 중에
	// "phandle", "linux,phandle" property를 가지는 node가 존재해야 값을 가짐 (fdt.c의 360 line 참고)
	// exynos5420 용 dts 파일들의 node를 찾아 본 결과 "phandle", "linux,phandle" property를
	// 가지는 node는 존재하지 않음
	// Reference폴더에 있는 Power_ePAPR_APPROVED_v1.1.pdf의 22page의 Programming Note를 참고하면
	// phandle값이 자동으로 삽입됨
	// 결국 np는 gic node의 주소가 됨

	// np: gic node의 주소
	// np: exynos5420 dtb상의 mct_map 의 주소
	of_node_get(np);

	raw_spin_unlock_irqrestore(&devtree_lock, flags);
	// devtree_lock을 사용j한 spin lock 해재하고 flags에 저장된 cpsr을 복원
	// devtree_lock을 사용j한 spin lock 해재하고 flags에 저장된 cpsr을 복원

	// np: gic node의 주소
	// np: exynos5420 dtb상의 mct_map 의 주소
	return np;
	// return gic node의 주소
	// return exynos5420 dtb상의 mct_map 의 주소
}
EXPORT_SYMBOL(of_find_node_by_phandle);

/**
 * of_find_property_value_of_size
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @len:	requested length of property value
 *
 * Search for a property in a device node and valid the requested size.
 * Returns the property value on success, -EINVAL if the property does not
 *  exist, -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 */
// ARM10C 20140215
// np: cpu0의 node의 주소값, propname: "reg", (sz * sizeof(*out_values)): 4
static void *of_find_property_value_of_size(const struct device_node *np,
			const char *propname, u32 len)
{
        // np: cpu0의 node의 주소값, propname: "reg", NULL
	struct property *prop = of_find_property(np, propname, NULL);
        // prop: cpu0의 reg property의 주소, (reg = <0x0>)

	if (!prop)
		return ERR_PTR(-EINVAL);

        // prop->value: reg의 값의 주소
	if (!prop->value)
		return ERR_PTR(-ENODATA);

        // len: 4, prop->length: 4
	if (len > prop->length)
		return ERR_PTR(-EOVERFLOW);

	return prop->value;
        // prop->value: reg의 값의 주소를 리턴
}

/**
 * of_property_read_u32_index - Find and read a u32 from a multi-value property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @index:	index of the u32 in the list of values
 * @out_value:	pointer to return value, modified only if no error.
 *
 * Search for a property in a device node and read nth 32-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u32 value can be decoded.
 */
int of_property_read_u32_index(const struct device_node *np,
				       const char *propname,
				       u32 index, u32 *out_value)
{
	const u32 *val = of_find_property_value_of_size(np, propname,
					((index + 1) * sizeof(*out_value)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = be32_to_cpup(((__be32 *)val) + index);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u32_index);

/**
 * of_property_read_u8_array - Find and read an array of u8 from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 8-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * dts entry of array should be like:
 *	property = /bits/ 8 <0x50 0x60 0x70>;
 *
 * The out_values is modified only if a valid u8 value can be decoded.
 */
int of_property_read_u8_array(const struct device_node *np,
			const char *propname, u8 *out_values, size_t sz)
{
	const u8 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	while (sz--)
		*out_values++ = *val++;
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u8_array);

/**
 * of_property_read_u16_array - Find and read an array of u16 from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 16-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * dts entry of array should be like:
 *	property = /bits/ 16 <0x5000 0x6000 0x7000>;
 *
 * The out_values is modified only if a valid u16 value can be decoded.
 */
int of_property_read_u16_array(const struct device_node *np,
			const char *propname, u16 *out_values, size_t sz)
{
	const __be16 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));

	if (IS_ERR(val))
		return PTR_ERR(val);

	while (sz--)
		*out_values++ = be16_to_cpup(val++);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u16_array);

/**
 * of_property_read_u32_array - Find and read an array of 32 bit integers
 * from a property.
 *
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_values:	pointer to return value, modified only if return value is 0.
 * @sz:		number of array elements to read
 *
 * Search for a property in a device node and read 32-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_values is modified only if a valid u32 value can be decoded.
 */
// ARM10C 20140215
// np: cpu0의 node의 주소값, propname: "reg", out_value: &hwid, 1
int of_property_read_u32_array(const struct device_node *np,
			       const char *propname, u32 *out_values,
			       size_t sz)
{
        // np: cpu0의 node의 주소값, propname: "reg", sizeof(*out_values)): 4, sz: 1
        // (sz * sizeof(*out_values)): 4
	const __be32 *val = of_find_property_value_of_size(np, propname,
						(sz * sizeof(*out_values)));
        // val: reg의 값의 주소

	if (IS_ERR(val))
		return PTR_ERR(val);

        // sz: 1
	while (sz--)
		*out_values++ = be32_to_cpup(val++);
                // *out_values: 0
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u32_array);

/**
 * of_property_read_u64 - Find and read a 64 bit integer from a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_value:	pointer to return value, modified only if return value is 0.
 *
 * Search for a property in a device node and read a 64-bit value from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_value is modified only if a valid u64 value can be decoded.
 */
int of_property_read_u64(const struct device_node *np, const char *propname,
			 u64 *out_value)
{
	const __be32 *val = of_find_property_value_of_size(np, propname,
						sizeof(*out_value));

	if (IS_ERR(val))
		return PTR_ERR(val);

	*out_value = of_read_number(val, 2);
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_u64);

/**
 * of_property_read_string - Find and read a string from a property
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @out_string:	pointer to null terminated return string, modified only if
 *		return value is 0.
 *
 * Search for a property in a device tree node and retrieve a null
 * terminated string value (pointer to data, not a copy). Returns 0 on
 * success, -EINVAL if the property does not exist, -ENODATA if property
 * does not have a value, and -EILSEQ if the string is not null-terminated
 * within the length of the property data.
 *
 * The out_string pointer is modified only if a valid string can be decoded.
 */
int of_property_read_string(struct device_node *np, const char *propname,
				const char **out_string)
{
	struct property *prop = of_find_property(np, propname, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	if (strnlen(prop->value, prop->length) >= prop->length)
		return -EILSEQ;
	*out_string = prop->value;
	return 0;
}
EXPORT_SYMBOL_GPL(of_property_read_string);

/**
 * of_property_read_string_index - Find and read a string from a multiple
 * strings property.
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 * @index:	index of the string in the list of strings
 * @out_string:	pointer to null terminated return string, modified only if
 *		return value is 0.
 *
 * Search for a property in a device tree node and retrieve a null
 * terminated string value (pointer to data, not a copy) in the list of strings
 * contained in that property.
 * Returns 0 on success, -EINVAL if the property does not exist, -ENODATA if
 * property does not have a value, and -EILSEQ if the string is not
 * null-terminated within the length of the property data.
 *
 * The out_string pointer is modified only if a valid string can be decoded.
 */
// ARM10C 20141018
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소, "reg-names", index: 0, , &name
// ARM10C 20141101
// dev: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소, "reg-names", index: 1, , &name
int of_property_read_string_index(struct device_node *np, const char *propname,
				  int index, const char **output)
{
	// np: devtree에서 allnext로 순회 하면서 찾은 gic node의 주소, propname: "reg-names"
	// of_find_property(devtree에서 allnext로 순회 하면서 찾은 gic node의 주소, "reg-names", NULL): NULL
	struct property *prop = of_find_property(np, propname, NULL);
	// prop: NULL
	int i = 0;
	size_t l = 0, total = 0;
	const char *p;

	// prop: NULL
	if (!prop)
		return -EINVAL;
		// return -EINVAL

	if (!prop->value)
		return -ENODATA;
	if (strnlen(prop->value, prop->length) >= prop->length)
		return -EILSEQ;

	p = prop->value;

	for (i = 0; total < prop->length; total += l, p += l) {
		l = strlen(p) + 1;
		if (i++ == index) {
			*output = p;
			return 0;
		}
	}
	return -ENODATA;
}
EXPORT_SYMBOL_GPL(of_property_read_string_index);

/**
 * of_property_match_string() - Find string in a list and return index
 * @np: pointer to node containing string list property
 * @propname: string list property name
 * @string: pointer to string to search for in string list
 *
 * This function searches a string list property and returns the index
 * of a specific string value.
 */
// ARM10C 20150321
// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, "clock-names", name: "fin_pll"
// ARM10C 20150328
// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, name: "mct"
int of_property_match_string(struct device_node *np, const char *propname,
			     const char *string)
{
	// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, propname: "clock-names"
	// of_find_property(devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, "clock-names", NULL):
	// mct node의 "clock-names" property의 주소
	struct property *prop = of_find_property(np, propname, NULL);
	// prop: mct node의 "clock-names" property의 주소

	size_t l;
	int i;
	const char *p, *end;

	// prop: mct node의 "clock-names" property의 주소
	if (!prop)
		return -EINVAL;

	// prop->value: (mct node의 "clock-names" property의 주소)->value: property "clock-names" 값의 시작 주소 ("fin_pll", "mct";)
	if (!prop->value)
		return -ENODATA;

	// prop->value: (mct node의 "clock-names" property의 주소)->value: property "clock-names" 값의 시작 주소 ("fin_pll", "mct";)
	p = prop->value;
	// p: property "clock-names" 값의 시작 주소 ("fin_pll", "mct")

	// p: property "clock-names" 값의 시작 주소 ("fin_pll", "mct")
	// prop->length: (mct node의 "clock-names" property의 주소)->length: 12
	end = p + prop->length;
	// end: property "clock-names" 값의 끝 주소

	// p: property "clock-names" 값의 시작 주소, end: property "clock-names" 값의 끝 주소
	for (i = 0; p < end; i++, p += l) {
		// i: 0, p: property "clock-names" 값의 시작 주소, strlen(property "clock-names" 값의 시작 주소): 7
		l = strlen(p) + 1;
		// l: 8

		// p: property "clock-names" 값의 시작 주소, l: 8, end: property "clock-names" 값의 끝 주소
		if (p + l > end)
			return -EILSEQ;

		// string: "fin_pll", p: property "clock-names" 값의 시작 주소
		pr_debug("comparing %s with %s\n", string, p);
		// "comparing fin_pll with fin_pll\n"

		// string: "fin_pll", p: property "clock-names" 값의 시작 주소 ("fin_pll")
		// strcmp("fin_pll", property "clock-names" 값의 시작 주소 ("fin_pll")): 0
		if (strcmp(string, p) == 0)
			// i: 0
			return i; /* Found it; return index */
			// return 0
	}
	return -ENODATA;
}
EXPORT_SYMBOL_GPL(of_property_match_string);

/**
 * of_property_count_strings - Find and return the number of strings from a
 * multiple strings property.
 * @np:		device node from which the property value is to be read.
 * @propname:	name of the property to be searched.
 *
 * Search for a property in a device tree node and retrieve the number of null
 * terminated string contain in it. Returns the number of strings on
 * success, -EINVAL if the property does not exist, -ENODATA if property
 * does not have a value, and -EILSEQ if the string is not null-terminated
 * within the length of the property data.
 */
int of_property_count_strings(struct device_node *np, const char *propname)
{
	struct property *prop = of_find_property(np, propname, NULL);
	int i = 0;
	size_t l = 0, total = 0;
	const char *p;

	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;
	if (strnlen(prop->value, prop->length) >= prop->length)
		return -EILSEQ;

	p = prop->value;

	for (i = 0; total < prop->length; total += l, p += l, i++)
		l = strlen(p) + 1;

	return i;
}
EXPORT_SYMBOL_GPL(of_property_count_strings);

void of_print_phandle_args(const char *msg, const struct of_phandle_args *args)
{
	int i;
	printk("%s %s", msg, of_node_full_name(args->np));
	for (i = 0; i < args->args_count; i++)
		printk(i ? ",%08x" : ":%08x", args->args[i]);
	printk("\n");
}

// ARM10C 20150321
// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, list_name: "clocks",
// cells_name: "#clock-cells", 0, index: 0, out_args: &clkspec
static int __of_parse_phandle_with_args(const struct device_node *np,
					const char *list_name,
					const char *cells_name,
					int cell_count, int index,
					struct of_phandle_args *out_args)
{
	const __be32 *list, *list_end;
	int rc = 0, size, cur_index = 0;
	// rc: 0, cur_index: 0

	uint32_t count = 0;
	// count: 0

	struct device_node *node = NULL;
	// node: NULL

	phandle phandle;

	/* Retrieve the phandle list property */
	// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, list_name: "clocks",
	// of_get_property(devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, "clocks", &size):
	// mct node의 "clocks" property의 값의 주소, size: 16
	list = of_get_property(np, list_name, &size);
	// list: mct node의 "clocks" property의 값의 주소

	// list: mct node의 "clocks" property의 값의 주소
	if (!list)
		return -ENOENT;

	// list: mct node의 "clocks" property의 값의 주소, size: 16, sizeof(*list): 4
	list_end = list + size / sizeof(*list);
	// list_end: mct node의 "clocks" property의 값의 주소 + 4

	/* Loop over the phandles until all the requested entry is found */
	// list: mct node의 "clocks" property의 값의 주소, list_end: mct node의 "clocks" property의 값의 주소 + 4
	while (list < list_end) {
		// EINVAL: 22
		rc = -EINVAL;
		// rc: -22

		count = 0;
		// count: 0

		/*
		 * If phandle is 0, then it is an empty entry with no
		 * arguments.  Skip forward to the next entry.
		 */
		// list: mct node의 "clocks" property의 값의 주소,
		// be32_to_cpup(mct node의 "clocks" property의 값의 주소): exynos5420 dtb상의 clock node의 주소
		phandle = be32_to_cpup(list++);
		// phandle: exynos5420 dtb상의 clock node의 주소

		// phandle: exynos5420 dtb상의 clock node의 주소
		if (phandle) {
			/*
			 * Find the provider node and parse the #*-cells
			 * property to determine the argument length.
			 *
			 * This is not needed if the cell count is hard-coded
			 * (i.e. cells_name not set, but cell_count is set),
			 * except when we're going to return the found node
			 * below.
			 */
			// cells_name: "#clock-cells", cur_index: 0, index: 0
			if (cells_name || cur_index == index) {
				// phandle: exynos5420 dtb상의 clock node의 주소
				// of_find_node_by_phandle(exynos5420 dtb상의 clock node의 주소): clock node의 주소
				node = of_find_node_by_phandle(phandle);
				// node: clock node의 주소

				// node: clock node의 주소
				if (!node) {
					pr_err("%s: could not find phandle\n",
						np->full_name);
					goto err;
				}
			}

			// cells_name: "#clock-cells"
			if (cells_name) {
				// node: clock node의 주소, cells_name: "#clock-cells"
				// of_property_read_u32(clock node의 주소, "#clock-cells", &count): 0, count: 1
				if (of_property_read_u32(node, cells_name,
							 &count)) {
					pr_err("%s: could not get %s for %s\n",
						np->full_name, cells_name,
						node->full_name);
					goto err;
				}
			} else {
				count = cell_count;
			}

			/*
			 * Make sure that the arguments actually fit in the
			 * remaining property data length
			 */
			// list: mct node의 "clocks" property의 값의 주소 + 1, count: 1,
			// list_end: mct node의 "clocks" property의 값의 주소 + 4
			if (list + count > list_end) {
				pr_err("%s: arguments longer than property\n",
					 np->full_name);
				goto err;
			}
		}

		/*
		 * All of the error cases above bail out of the loop, so at
		 * this point, the parsing is successful. If the requested
		 * index matches, then fill the out_args structure and return,
		 * or return -ENOENT for an empty entry.
		 */
		// ENOENT: 2
		rc = -ENOENT;
		// rc: -2

		// cur_index: 0, index: 0
		if (cur_index == index) {
			// phandle: exynos5420 dtb상의 clock node의 주소
			if (!phandle)
				goto err;

			// out_args: &clkspec
			if (out_args) {
				int i;

				// count: 1, MAX_PHANDLE_ARGS: 8
				if (WARN_ON(count > MAX_PHANDLE_ARGS))
					count = MAX_PHANDLE_ARGS;

				// out_args->np: (&clkspec)->np, node: clock node의 주소
				out_args->np = node;
				// out_args->np: (&clkspec)->np: clock node의 주소

				// out_args->args_count: (&clkspec)->args_count, count: 1
				out_args->args_count = count;
				// out_args->args_count: (&clkspec)->args_count: 1

				// count: 1
				for (i = 0; i < count; i++)
					// i: 0, out_args->args[0]: (&clkspec)->args[0]
					// list: mct node의 "clocks" property의 값의 주소 + 1
					// be32_to_cpup(mct node의 "clocks" property의 값의 주소 + 1): 1
					out_args->args[i] = be32_to_cpup(list++);
					// out_args->args[0]: (&clkspec)->args[0]: 1
			} else {
				of_node_put(node);
			}

			/* Found it! return success */
			return 0;
			// return 0
		}

		of_node_put(node);
		node = NULL;
		list += count;
		cur_index++;
	}

	/*
	 * Unlock node before returning result; will be one of:
	 * -ENOENT : index is for empty phandle
	 * -EINVAL : parsing error on data
	 * [1..n]  : Number of phandle (count mode; when index = -1)
	 */
	rc = index < 0 ? cur_index : -ENOENT;
 err:
	if (node)
		of_node_put(node);
	return rc;
}

/**
 * of_parse_phandle - Resolve a phandle property to a device_node pointer
 * @np: Pointer to device node holding phandle property
 * @phandle_name: Name of property holding a phandle value
 * @index: For properties holding a table of phandles, this is the index into
 *         the table
 *
 * Returns the device_node pointer with refcount incremented.  Use
 * of_node_put() on it when done.
 */
struct device_node *of_parse_phandle(const struct device_node *np,
				     const char *phandle_name, int index)
{
	struct of_phandle_args args;

	if (index < 0)
		return NULL;

	if (__of_parse_phandle_with_args(np, phandle_name, NULL, 0,
					 index, &args))
		return NULL;

	return args.np;
}
EXPORT_SYMBOL(of_parse_phandle);

/**
 * of_parse_phandle_with_args() - Find a node pointed by phandle in a list
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cells_name:	property name that specifies phandles' arguments count
 * @index:	index of a phandle to parse out
 * @out_args:	optional pointer to output arguments structure (will be filled)
 *
 * This function is useful to parse lists of phandles and their arguments.
 * Returns 0 on success and fills out_args, on error returns appropriate
 * errno value.
 *
 * Caller is responsible to call of_node_put() on the returned out_args->node
 * pointer.
 *
 * Example:
 *
 * phandle1: node1 {
 * 	#list-cells = <2>;
 * }
 *
 * phandle2: node2 {
 * 	#list-cells = <1>;
 * }
 *
 * node3 {
 * 	list = <&phandle1 1 2 &phandle2 3>;
 * }
 *
 * To get a device_node of the `node2' node you may call this:
 * of_parse_phandle_with_args(node3, "list", "#list-cells", 1, &args);
 */
// ARM10C 20150321
// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, "clocks", "#clock-cells", index: 0, &clkspec
int of_parse_phandle_with_args(const struct device_node *np, const char *list_name,
				const char *cells_name, int index,
				struct of_phandle_args *out_args)
{
	// index: 0
	if (index < 0)
		return -EINVAL;

	// np: devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, list_name: "clocks",
	// cells_name: "#clock-cells", index: 0, out_args: &clkspec
	// __of_parse_phandle_with_args(devtree에서 allnext로 순회 하면서 찾은 mct node의 주소, "clocks", "#clock-cells", 0, 0, &clkspec): 0
	return __of_parse_phandle_with_args(np, list_name, cells_name, 0,
					    index, out_args);
	// return 0

	// __of_parse_phandle_with_args에서 한일:
	// mct node 에서 "clocks" property의 이용하여 devtree의 값을 파싱하여 clkspec에 값을 가져옴
	//
	// (&clkspec)->np: clock node의 주소
	// (&clkspec)->args_count: 1
	// (&clkspec)->args[0]: 1
}
EXPORT_SYMBOL(of_parse_phandle_with_args);

/**
 * of_parse_phandle_with_fixed_args() - Find a node pointed by phandle in a list
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cell_count: number of argument cells following the phandle
 * @index:	index of a phandle to parse out
 * @out_args:	optional pointer to output arguments structure (will be filled)
 *
 * This function is useful to parse lists of phandles and their arguments.
 * Returns 0 on success and fills out_args, on error returns appropriate
 * errno value.
 *
 * Caller is responsible to call of_node_put() on the returned out_args->node
 * pointer.
 *
 * Example:
 *
 * phandle1: node1 {
 * }
 *
 * phandle2: node2 {
 * }
 *
 * node3 {
 * 	list = <&phandle1 0 2 &phandle2 2 3>;
 * }
 *
 * To get a device_node of the `node2' node you may call this:
 * of_parse_phandle_with_fixed_args(node3, "list", 2, 1, &args);
 */
int of_parse_phandle_with_fixed_args(const struct device_node *np,
				const char *list_name, int cell_count,
				int index, struct of_phandle_args *out_args)
{
	if (index < 0)
		return -EINVAL;
	return __of_parse_phandle_with_args(np, list_name, NULL, cell_count,
					   index, out_args);
}
EXPORT_SYMBOL(of_parse_phandle_with_fixed_args);

/**
 * of_count_phandle_with_args() - Find the number of phandles references in a property
 * @np:		pointer to a device tree node containing a list
 * @list_name:	property name that contains a list
 * @cells_name:	property name that specifies phandles' arguments count
 *
 * Returns the number of phandle + argument tuples within a property. It
 * is a typical pattern to encode a list of phandle and variable
 * arguments into a single property. The number of arguments is encoded
 * by a property in the phandle-target node. For example, a gpios
 * property would contain a list of GPIO specifies consisting of a
 * phandle and 1 or more arguments. The number of arguments are
 * determined by the #gpio-cells property in the node pointed to by the
 * phandle.
 */
int of_count_phandle_with_args(const struct device_node *np, const char *list_name,
				const char *cells_name)
{
	return __of_parse_phandle_with_args(np, list_name, cells_name, 0, -1,
					    NULL);
}
EXPORT_SYMBOL(of_count_phandle_with_args);

#if defined(CONFIG_OF_DYNAMIC)
static int of_property_notify(int action, struct device_node *np,
			      struct property *prop)
{
	struct of_prop_reconfig pr;

	pr.dn = np;
	pr.prop = prop;
	return of_reconfig_notify(action, &pr);
}
#else
static int of_property_notify(int action, struct device_node *np,
			      struct property *prop)
{
	return 0;
}
#endif

/**
 * of_add_property - Add a property to a node
 */
int of_add_property(struct device_node *np, struct property *prop)
{
	struct property **next;
	unsigned long flags;
	int rc;

	rc = of_property_notify(OF_RECONFIG_ADD_PROPERTY, np, prop);
	if (rc)
		return rc;

	prop->next = NULL;
	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = &np->properties;
	while (*next) {
		if (strcmp(prop->name, (*next)->name) == 0) {
			/* duplicate ! don't insert it */
			raw_spin_unlock_irqrestore(&devtree_lock, flags);
			return -1;
		}
		next = &(*next)->next;
	}
	*next = prop;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

#ifdef CONFIG_PROC_DEVICETREE
	/* try to add to proc as well if it was initialized */
	if (np->pde)
		proc_device_tree_add_prop(np->pde, prop);
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}

/**
 * of_remove_property - Remove a property from a node.
 *
 * Note that we don't actually remove it, since we have given out
 * who-knows-how-many pointers to the data using get-property.
 * Instead we just move the property to the "dead properties"
 * list, so it won't be found any more.
 */
int of_remove_property(struct device_node *np, struct property *prop)
{
	struct property **next;
	unsigned long flags;
	int found = 0;
	int rc;

	rc = of_property_notify(OF_RECONFIG_REMOVE_PROPERTY, np, prop);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = &np->properties;
	while (*next) {
		if (*next == prop) {
			/* found the node */
			*next = prop->next;
			prop->next = np->deadprops;
			np->deadprops = prop;
			found = 1;
			break;
		}
		next = &(*next)->next;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!found)
		return -ENODEV;

#ifdef CONFIG_PROC_DEVICETREE
	/* try to remove the proc node as well */
	if (np->pde)
		proc_device_tree_remove_prop(np->pde, prop);
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}

/*
 * of_update_property - Update a property in a node, if the property does
 * not exist, add it.
 *
 * Note that we don't actually remove it, since we have given out
 * who-knows-how-many pointers to the data using get-property.
 * Instead we just move the property to the "dead properties" list,
 * and add the new property to the property list
 */
int of_update_property(struct device_node *np, struct property *newprop)
{
	struct property **next, *oldprop;
	unsigned long flags;
	int rc, found = 0;

	rc = of_property_notify(OF_RECONFIG_UPDATE_PROPERTY, np, newprop);
	if (rc)
		return rc;

	if (!newprop->name)
		return -EINVAL;

	oldprop = of_find_property(np, newprop->name, NULL);
	if (!oldprop)
		return of_add_property(np, newprop);

	raw_spin_lock_irqsave(&devtree_lock, flags);
	next = &np->properties;
	while (*next) {
		if (*next == oldprop) {
			/* found the node */
			newprop->next = oldprop->next;
			*next = newprop;
			oldprop->next = np->deadprops;
			np->deadprops = oldprop;
			found = 1;
			break;
		}
		next = &(*next)->next;
	}
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	if (!found)
		return -ENODEV;

#ifdef CONFIG_PROC_DEVICETREE
	/* try to add to proc as well if it was initialized */
	if (np->pde)
		proc_device_tree_update_prop(np->pde, newprop, oldprop);
#endif /* CONFIG_PROC_DEVICETREE */

	return 0;
}

#if defined(CONFIG_OF_DYNAMIC)
/*
 * Support for dynamic device trees.
 *
 * On some platforms, the device tree can be manipulated at runtime.
 * The routines in this section support adding, removing and changing
 * device tree nodes.
 */

static BLOCKING_NOTIFIER_HEAD(of_reconfig_chain);

int of_reconfig_notifier_register(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_register);

int of_reconfig_notifier_unregister(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&of_reconfig_chain, nb);
}
EXPORT_SYMBOL_GPL(of_reconfig_notifier_unregister);

int of_reconfig_notify(unsigned long action, void *p)
{
	int rc;

	rc = blocking_notifier_call_chain(&of_reconfig_chain, action, p);
	return notifier_to_errno(rc);
}

#ifdef CONFIG_PROC_DEVICETREE
static void of_add_proc_dt_entry(struct device_node *dn)
{
	struct proc_dir_entry *ent;

	ent = proc_mkdir(strrchr(dn->full_name, '/') + 1, dn->parent->pde);
	if (ent)
		proc_device_tree_add_node(dn, ent);
}
#else
static void of_add_proc_dt_entry(struct device_node *dn)
{
	return;
}
#endif

/**
 * of_attach_node - Plug a device node into the tree and global list.
 */
int of_attach_node(struct device_node *np)
{
	unsigned long flags;
	int rc;

	rc = of_reconfig_notify(OF_RECONFIG_ATTACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);
	np->sibling = np->parent->child;
	np->allnext = of_allnodes;
	np->parent->child = np;
	of_allnodes = np;
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_add_proc_dt_entry(np);
	return 0;
}

#ifdef CONFIG_PROC_DEVICETREE
static void of_remove_proc_dt_entry(struct device_node *dn)
{
	proc_remove(dn->pde);
}
#else
static void of_remove_proc_dt_entry(struct device_node *dn)
{
	return;
}
#endif

/**
 * of_detach_node - "Unplug" a node from the device tree.
 *
 * The caller must hold a reference to the node.  The memory associated with
 * the node is not freed until its refcount goes to zero.
 */
int of_detach_node(struct device_node *np)
{
	struct device_node *parent;
	unsigned long flags;
	int rc = 0;

	rc = of_reconfig_notify(OF_RECONFIG_DETACH_NODE, np);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&devtree_lock, flags);

	if (of_node_check_flag(np, OF_DETACHED)) {
		/* someone already detached it */
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
		return rc;
	}

	parent = np->parent;
	if (!parent) {
		raw_spin_unlock_irqrestore(&devtree_lock, flags);
		return rc;
	}

	if (of_allnodes == np)
		of_allnodes = np->allnext;
	else {
		struct device_node *prev;
		for (prev = of_allnodes;
		     prev->allnext != np;
		     prev = prev->allnext)
			;
		prev->allnext = np->allnext;
	}

	if (parent->child == np)
		parent->child = np->sibling;
	else {
		struct device_node *prevsib;
		for (prevsib = np->parent->child;
		     prevsib->sibling != np;
		     prevsib = prevsib->sibling)
			;
		prevsib->sibling = np->sibling;
	}

	of_node_set_flag(np, OF_DETACHED);
	raw_spin_unlock_irqrestore(&devtree_lock, flags);

	of_remove_proc_dt_entry(np);
	return rc;
}
#endif /* defined(CONFIG_OF_DYNAMIC) */

// ARM10C 20140215
// ap : 할당받은 메모리의 시작 주소, np : pinctrl@13400000 값으로 찾은 node tree의 주소
// id : 0, start: "pinctrl0", len : 7
static void of_alias_add(struct alias_prop *ap, struct device_node *np,
			 int id, const char *stem, int stem_len)
{
	ap->np = np;
        // ap-np: pinctrl@13400000 값으로 찾은 node tree의 주소

	ap->id = id;
        // ap->id: 0

        // stem: "pinctrl0", stem_len: 7
	strncpy(ap->stem, stem, stem_len);
	ap->stem[stem_len] = 0;
        // ap->stem: "pinctrl"

	list_add_tail(&ap->link, &aliases_lookup);
	pr_debug("adding DT alias:%s: stem=%s id=%i node=%s\n",
		 ap->alias, ap->stem, ap->id, of_node_full_name(np));
}

/**
 * of_alias_scan - Scan all properties of 'aliases' node
 *
 * The function scans all the properties of 'aliases' node and populate
 * the the global lookup table with the properties.  It returns the
 * number of alias_prop found, or error code in error case.
 *
 * @dt_alloc:	An allocator that provides a virtual address to memory
 *		for the resulting tree
 */
// ARM10C 20140208
// dt_alloc : early_init_dt_alloc_memory_arch
void of_alias_scan(void * (*dt_alloc)(u64 size, u64 align))
{
	struct property *pp;

	of_chosen = of_find_node_by_path("/chosen");
	// of_chosen : chosen 노드의 struct device_node 주소
	if (of_chosen == NULL)
		of_chosen = of_find_node_by_path("/chosen@0");

	if (of_chosen) {
		const char *name;

		name = of_get_property(of_chosen, "linux,stdout-path", NULL);
		if (name)
			of_stdout = of_find_node_by_path(name);
	}

	of_aliases = of_find_node_by_path("/aliases");
	// of_aliases : aliases 노드의 struct device_node 주소

	if (!of_aliases)
		return;

	for_each_property_of_node(of_aliases, pp) {
		const char *start = pp->name;
		// pp->name : "pinctrl0"
		const char *end = start + strlen(start);
		struct device_node *np;
		struct alias_prop *ap;
		int id, len;

		/* Skip those we do not want to proceed */
		if (!strcmp(pp->name, "name") ||
		    !strcmp(pp->name, "phandle") ||
		    !strcmp(pp->name, "linux,phandle"))
			continue;

		np = of_find_node_by_path(pp->value);
                // pp->value값으로 node의 주소위치를 찾음
		// np : pinctrl@13400000 값으로 찾은 node tree의 주소

		if (!np)
			continue;

		/* walk the alias backwards to extract the id and work out
		 * the 'stem' string */
		while (isdigit(*(end-1)) && end > start)
			end--;
		len = end - start;
		// 숫자 값은 빼고 이름 길이만 남김
		// len : 7

		if (kstrtoint(end, 10, &id) < 0)
			// id : 0
			continue;

		/* Allocate an alias_prop with enough space for the stem */
		// sizeof(*ap) + len + 1 : 32
		ap = dt_alloc(sizeof(*ap) + len + 1, 4);
		// ap : 할당받은 메모리의 시작 주소

		if (!ap)
			continue;
		memset(ap, 0, sizeof(*ap) + len + 1);
		ap->alias = start;
		// ap->alias : pp->name : "pinctrl0" 의시작 주소
// 2014/02/08 종료
// 2014/02/15 시작
		// ap : 할당받은 메모리의 시작 주소, np : pinctrl@13400000 값으로 찾은 node tree의 주소
                // id : 0, start: "pinctrl0", len : 7
		of_alias_add(ap, np, id, start, len);
                // aliases에 노드에 있는 node value를가지고 해당 node를 찾고 aliases_lookup에 리스트를 생성함
	}
}

/**
 * of_alias_get_id - Get alias id for the given device_node
 * @np:		Pointer to the given device_node
 * @stem:	Alias stem of the given device_node
 *
 * The function travels the lookup table to get alias id for the given
 * device_node and alias stem.  It returns the alias id if find it.
 */
int of_alias_get_id(struct device_node *np, const char *stem)
{
	struct alias_prop *app;
	int id = -ENODEV;

	mutex_lock(&of_aliases_mutex);
	list_for_each_entry(app, &aliases_lookup, link) {
		if (strcmp(app->stem, stem) != 0)
			continue;

		if (np == app->np) {
			id = app->id;
			break;
		}
	}
	mutex_unlock(&of_aliases_mutex);

	return id;
}
EXPORT_SYMBOL_GPL(of_alias_get_id);

const __be32 *of_prop_next_u32(struct property *prop, const __be32 *cur,
			       u32 *pu)
{
	const void *curv = cur;

	if (!prop)
		return NULL;

	if (!cur) {
		curv = prop->value;
		goto out_val;
	}

	curv += sizeof(*cur);
	if (curv >= prop->value + prop->length)
		return NULL;

out_val:
	*pu = be32_to_cpup(curv);
	return curv;
}
EXPORT_SYMBOL_GPL(of_prop_next_u32);

const char *of_prop_next_string(struct property *prop, const char *cur)
{
	const void *curv = cur;

	if (!prop)
		return NULL;

	if (!cur)
		return prop->value;

	curv += strlen(cur) + 1;
	if (curv >= prop->value + prop->length)
		return NULL;

	return curv;
}
EXPORT_SYMBOL_GPL(of_prop_next_string);

/**
 * of_device_is_stdout_path - check if a device node matches the
 *                            linux,stdout-path property
 *
 * Check if this device node matches the linux,stdout-path property
 * in the chosen node. return true if yes, false otherwise.
 */
int of_device_is_stdout_path(struct device_node *dn)
{
	if (!of_stdout)
		return false;

	return of_stdout == dn;
}
EXPORT_SYMBOL_GPL(of_device_is_stdout_path);

/**
 *	of_find_next_cache_node - Find a node's subsidiary cache
 *	@np:	node of type "cpu" or "cache"
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.  Caller should hold a reference
 *	to np.
 */
struct device_node *of_find_next_cache_node(const struct device_node *np)
{
	struct device_node *child;
	const phandle *handle;

	handle = of_get_property(np, "l2-cache", NULL);
	if (!handle)
		handle = of_get_property(np, "next-level-cache", NULL);

	if (handle)
		return of_find_node_by_phandle(be32_to_cpup(handle));

	/* OF on pmac has nodes instead of properties named "l2-cache"
	 * beneath CPU nodes.
	 */
	if (!strcmp(np->type, "cpu"))
		for_each_child_of_node(np, child)
			if (!strcmp(child->type, "cache"))
				return child;

	return NULL;
}
