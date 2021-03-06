/*
 * /proc/sys support
 */
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/printk.h>
#include <linux/security.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/module.h>
#include "internal.h"

static const struct dentry_operations proc_sys_dentry_operations;
static const struct file_operations proc_sys_file_operations;
static const struct inode_operations proc_sys_inode_operations;
static const struct file_operations proc_sys_dir_file_operations;
static const struct inode_operations proc_sys_dir_operations;

void proc_sys_poll_notify(struct ctl_table_poll *poll)
{
	if (!poll)
		return;

	atomic_inc(&poll->event);
	wake_up_interruptible(&poll->wait);
}

static struct ctl_table root_table[] = {
	{
		.procname = "",
		.mode = S_IFDIR|S_IRUGO|S_IXUGO,
	},
	{ }
};
// ARM10C 20160611
// ARM10C 20160625
// ARM10C 20160702
static struct ctl_table_root sysctl_table_root = {
	.default_set.dir.header = {
		{{.count = 1,
		  .nreg = 1,
		  .ctl_table = root_table }},
		.ctl_table_arg = root_table,
		.root = &sysctl_table_root,
		.set = &sysctl_table_root.default_set,
	},
};

// ARM10C 20160702
// DEFINE_SPINLOCK(sysctl_lock):
// spinlock_t sysctl_lock =
// (spinlock_t )
// { { .rlock =
//     {
//       .raw_lock = { { 0 } },
//       .magic = 0xdead4ead,
//       .owner_cpu = -1,
//       .owner = 0xffffffff,
//     }
// } }
static DEFINE_SPINLOCK(sysctl_lock);

static void drop_sysctl_table(struct ctl_table_header *header);
static int sysctl_follow_link(struct ctl_table_header **phead,
	struct ctl_table **pentry, struct nsproxy *namespaces);
static int insert_links(struct ctl_table_header *head);
static void put_links(struct ctl_table_header *header);

static void sysctl_print_dir(struct ctl_dir *dir)
{
	if (dir->header.parent)
		sysctl_print_dir(dir->header.parent);
	pr_cont("%s/", dir->header.ctl_table[0].procname);
}

// ARM10C 20160709
// name: "sched_min_granularity_ns", namelen: 24, parent_name: "sched_child_runs_first", strlen("sched_child_runs_first"): 21
static int namecmp(const char *name1, int len1, const char *name2, int len2)
{
	int minlen;
	int cmp;

	// len1: 24
	minlen = len1;
	// minlen: 24

	// minlen: 24, len2: 21
	if (minlen > len2)
		// minlen: 24, len2: 21
		minlen = len2;
		// minlen: 21

	// name1: "sched_min_granularity_ns", name2: "sched_child_runs_first", minlen: 21
	// memcmp("sched_min_granularity_ns", "sched_child_runs_first", 21): 8
	cmp = memcmp(name1, name2, minlen);
	// cmp: 8

	// cmp: 8
	if (cmp == 0)
		cmp = len1 - len2;

	// cmp: 8
	return cmp;
	// return 8
}

/* Called under sysctl_lock */
// ARM10C 20160702
// &head, dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
static struct ctl_table *find_entry(struct ctl_table_header **phead,
	struct ctl_dir *dir, const char *name, int namelen)
{
	struct ctl_table_header *head;
	struct ctl_table *entry;

	// dir->root.rb_node: (&(&sysctl_table_root.default_set)->dir)->root.rb_node: NULL
	struct rb_node *node = dir->root.rb_node;
	// node: NULL

	// node: NULL
	while (node)
	{
		struct ctl_node *ctl_node;
		const char *procname;
		int cmp;

		ctl_node = rb_entry(node, struct ctl_node, node);
		head = ctl_node->header;
		entry = &head->ctl_table[ctl_node - head->node];
		procname = entry->procname;

		cmp = namecmp(name, namelen, procname, strlen(procname));
		if (cmp < 0)
			node = node->rb_left;
		else if (cmp > 0)
			node = node->rb_right;
		else {
			*phead = head;
			return entry;
		}
	}
	return NULL;
	// return NULL
}

// ARM10C 20160702
// header: &(kmem_cache#29-oX)->header, entry: (kmem_cache#29-oX + 52) (struct ctl_table)
// ARM10C 20160709
// [2nd][f1] header: kmem_cache#25-oX, entry: kmem_cache#24-oX (struct ctl_table)
// ARM10C 20160709
// [2nd][f2] header: kmem_cache#25-oX, entry: (kmem_cache#24-oX (struct ctl_table))[1]
static int insert_entry(struct ctl_table_header *head, struct ctl_table *entry)
{
	// entry: (kmem_cache#29-oX + 52) (struct ctl_table),
	// head->ctl_table: (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table),
	// head->node: (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node),
	// &head->node[0].node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
	// entry: kmem_cache#24-oX (struct ctl_table),
	// head->ctl_table: (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX (struct ctl_table),
	// head->node: (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node),
	// &head->node[0].node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// entry: (kmem_cache#24-oX (struct ctl_table))[1],
	// head->ctl_table: (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX (struct ctl_table),
	// head->node: (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node),
	// &head->node[1].node: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
	struct rb_node *node = &head->node[entry - head->ctl_table].node;
	// node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
	// node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// node: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node

	// head->parent: (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	// &head->parent->root.rb_node: &(&(&sysctl_table_root.default_set)->dir)->root.rb_node
	// head->parent: (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	// &head->parent->root.rb_node: &(kmem_cache#29-oX)->root.rb_node
	// head->parent: (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	// &head->parent->root.rb_node: &(kmem_cache#29-oX)->root.rb_node
	struct rb_node **p = &head->parent->root.rb_node;
	// p: &(&(&sysctl_table_root.default_set)->dir)->root.rb_node
	// p: &(kmem_cache#29-oX)->root.rb_node
	// p: &(kmem_cache#29-oX)->root.rb_node

	struct rb_node *parent = NULL;
	// parent: NULL
	// parent: NULL
	// parent: NULL

	// entry->procname: ((kmem_cache#29-oX + 52) (struct ctl_table))->procname: (kmem_cache#29-oX + 120): "kernel"
	// entry->procname: ((kmem_cache#24-oX (struct ctl_table))[0])->procname: "sched_child_runs_first"
	// entry->procname: ((kmem_cache#24-oX (struct ctl_table))[1])->procname: "sched_min_granularity_ns"
	const char *name = entry->procname;
	// name: (kmem_cache#29-oX + 120): "kernel"
	// name: "sched_child_runs_first"
	// name: "sched_min_granularity_ns"

	// name: (kmem_cache#29-oX + 120): "kernel", strlen("kernel"): 6
	// name: "sched_child_runs_first", strlen("sched_child_runs_first"): 21
	// name: "sched_min_granularity_ns", strlen("sched_min_granularity_ns"): 24
	int namelen = strlen(name);
	// namelen: 6
	// namelen: 21
	// namelen: 24

	// *p: (&(&sysctl_table_root.default_set)->dir)->root.rb_node: NULL
	// *p: (kmem_cache#29-oX)->root.rb_node: NULL
	// *p: (kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	while (*p) {
		struct ctl_table_header *parent_head;
		struct ctl_table *parent_entry;
		struct ctl_node *parent_node;
		const char *parent_name;
		int cmp;

		// parent: NULL, *p: (kmem_cache#29-oX)->root.rb_node: (&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		parent = *p;
		// parent: (&(kmem_cache#25-oX)[1] (struct ctl_node)).node

		// parent: (&(kmem_cache#25-oX)[1] (struct ctl_node)).node, node: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
		// rb_entry((&(kmem_cache#25-oX)[1] (struct ctl_node)).node, struct ctl_node, &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node):
		// &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		parent_node = rb_entry(parent, struct ctl_node, node);
		// parent_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node

		// parent_node->header: (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->header: kmem_cache#25-oX
		parent_head = parent_node->header;
		// parent_head: kmem_cache#25-oX

		// parent_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node,
		// parent_head->node: (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
		// &parent_head->ctl_table[0]: &(kmem_cache#25-oX)->ctl_table[0]: &(kmem_cache#24-oX [0])
		parent_entry = &parent_head->ctl_table[parent_node - parent_head->node];
		// parent_entry: &(kmem_cache#24-oX [0])

		// parent_entry->procname: (&(kmem_cache#24-oX [0]))->procname: "sched_child_runs_first"
		parent_name = parent_entry->procname;
		// parent_name: "sched_child_runs_first"

		// name: "sched_min_granularity_ns", namelen: 24, parent_name: "sched_child_runs_first", strlen("sched_child_runs_first"): 21
		// namecmp("sched_min_granularity_ns", 24, "sched_child_runs_first", 21): 8
		cmp = namecmp(name, namelen, parent_name, strlen(parent_name));
		// cmp: 8

		// cmp: 8
		if (cmp < 0)
			p = &(*p)->rb_left;
		else if (cmp > 0)
			// &(*p)->rb_right: &(&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right
			p = &(*p)->rb_right;
			// p: &(&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right
		else {
			pr_err("sysctl duplicate entry: ");
			sysctl_print_dir(head->parent);
			pr_cont("/%s\n", entry->procname);
			return -EEXIST;
		}
	}

	// node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node, parent: NULL,
	// p: &(&(&sysctl_table_root.default_set)->dir)->root.rb_node
	// node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node, parent: NULL
	// p: &(kmem_cache#29-oX)->root.rb_node
	// node: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node, parent: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// p: &(&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right
	rb_link_node(node, parent, p);

	// rb_link_node 에서 한일:
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
	// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node

	// rb_link_node 에서 한일:
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
	// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node

	// rb_link_node 에서 한일:
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node

	// node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node,
	// head->parent: (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir,
	// &head->parent->root: &(&(&sysctl_table_root.default_set)->dir)->root
	// node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node,
	// head->parent: (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	// &head->parent->root: &(kmem_cache#29-oX)->root
	// node: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node,
	// head->parent: (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	// &head->parent->root: &(kmem_cache#29-oX)->root
	rb_insert_color(node, &head->parent->root);

	// rb_insert_color 에서 한일:
	// &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 node 로 추가후 rbtree 로 구성
	// (proc의 kernel directory)
	/*
	//                          proc-b
	//                         (kernel)
	*/

	// rb_insert_color 에서 한일:
	// &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node 을 node 로 추가 후 rbtree 로 구성
	// (kern_table 의 1 번째 index의 값)
	/*
	//                       kern_table-b
	//                 (sched_child_runs_first)
	*/

	// rb_insert_color 에서 한일:
	// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
	// (kern_table 의 2 번째 index의 값)
	/*
	//                       kern_table-b
	//                 (sched_child_runs_first)
	//                                      \
	//                                        kern_table-r
	//                                  (sched_min_granularity_ns)
	//
	*/

	return 0;
	// return 0
	// return 0
	// return 0
}

static void erase_entry(struct ctl_table_header *head, struct ctl_table *entry)
{
	struct rb_node *node = &head->node[entry - head->ctl_table].node;

	rb_erase(node, &head->parent->root);
}

// ARM10C 20160702
// header: kmem_cache#25-oX, root: &sysctl_table_root, set: &sysctl_table_root.default_set,
// node: &(kmem_cache#25-oX)[1] (struct ctl_node), table: kmem_cache#24-oX
// ARM10C 20160702
// &new->header: &(kmem_cache#29-oX)->header, set->dir.header.root: (&sysctl_table_root.default_set)->dir.header.root,
// set: &sysctl_table_root.default_set, node: (kmem_cache#29-oX + 36) (struct ctl_node), table: (kmem_cache#29-oX + 52) (struct ctl_table)
static void init_header(struct ctl_table_header *head,
	struct ctl_table_root *root, struct ctl_table_set *set,
	struct ctl_node *node, struct ctl_table *table)
{
	// head->ctl_table: (kmem_cache#25-oX)->ctl_table, table: kmem_cache#24-oX
	head->ctl_table = table;
	// head->ctl_table: (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX

	// head->ctl_table_arg: (kmem_cache#25-oX)->ctl_table_arg, table: kmem_cache#24-oX
	head->ctl_table_arg = table;
	// head->ctl_table_arg: (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX

	// head->used: (kmem_cache#25-oX)->used
	head->used = 0;
	// head->used: (kmem_cache#25-oX)->used: 0

	// head->count: (kmem_cache#25-oX)->count
	head->count = 1;
	// head->count: (kmem_cache#25-oX)->count: 1

	// head->nreg: (kmem_cache#25-oX)->nreg
	head->nreg = 1;
	// head->nreg: (kmem_cache#25-oX)->nreg: 1

	// head->unregistering: (kmem_cache#25-oX)->unregistering
	head->unregistering = NULL;
	// head->unregistering: (kmem_cache#25-oX)->unregistering: NULL

	// head->root: (kmem_cache#25-oX)->root, root: &sysctl_table_root
	head->root = root;
	// head->root: (kmem_cache#25-oX)->root: &sysctl_table_root

	// head->set: (kmem_cache#25-oX)->set, set: &sysctl_table_root.default_set
	head->set = set;
	// head->set: (kmem_cache#25-oX)->set: &sysctl_table_root.default_set

	// head->parent: (kmem_cache#25-oX)->parent
	head->parent = NULL;
	// head->parent: (kmem_cache#25-oX)->parent: NULL

	// head->node: (kmem_cache#25-oX)->node, node: &(kmem_cache#25-oX)[1] (struct ctl_node)
	head->node = node;
	// head->node: (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)

	// node: &(kmem_cache#25-oX)[1] (struct ctl_node)
	if (node) {
		struct ctl_table *entry;

		// table: kmem_cache#24-oX, entry: kmem_cache#24-oX,
		// entry->procname: (kmem_cache#24-oX)->procname: "sched_child_runs_first"
		for (entry = table; entry->procname; entry++, node++)
			// entry: kmem_cache#24-oX, node: &(kmem_cache#25-oX)[1] (struct ctl_node)

			// node->header: (&(kmem_cache#25-oX)[1] (struct ctl_node))->header, header: kmem_cache#25-oX
			node->header = head;
			// node->header: (&(kmem_cache#25-oX)[1] (struct ctl_node))->header: kmem_cache#25-oX

			// entry: kmem_cache#24-oX (kern_table) 의 child 없는 맴버 46개 만큼 위 loop를 수행
	}
}

static void erase_header(struct ctl_table_header *head)
{
	struct ctl_table *entry;
	for (entry = head->ctl_table; entry->procname; entry++)
		erase_entry(head, entry);
}

// ARM10C 20160702
// [1st] dir: &(&sysctl_table_root.default_set)->dir, &new->header: &(kmem_cache#29-oX)->header
// ARM10C 20160709
// [2nd] dir: kmem_cache#29-oX, header: kmem_cache#25-oX
static int insert_header(struct ctl_dir *dir, struct ctl_table_header *header)
{
	struct ctl_table *entry;
	int err;

	// [1st] dir->header.nreg: (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
	// [2nd] dir->header.nreg: (kmem_cache#29-oX)->header.nreg: 1
	dir->header.nreg++;
	// [1st] dir->header.nreg: (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
	// [2nd] dir->header.nreg: (kmem_cache#29-oX)->header.nreg: 2

	// [1st] header->parent: (&(kmem_cache#29-oX)->header)->parent: NULL,
	// [1st] dir: &(&sysctl_table_root.default_set)->dir
	// [2nd] header->parent: (kmem_cache#25-oX)->parent: NULL, dir: kmem_cache#29-oX
	header->parent = dir;
	// [1st] header->parent: (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	// [2nd] header->parent: (kmem_cache#25-oX)->parent: kmem_cache#29-oX

	// [1st] header: &(kmem_cache#29-oX)->header, insert_links(&(kmem_cache#29-oX)->header): 0
	// [2nd] header: kmem_cache#25-oX, insert_links(kmem_cache#25-oX): 0
	err = insert_links(header);
	// [1st] err: 0
	// [2nd] err: 0

	// [1st] err: 0
	// [2nd] err: 0
	if (err)
		goto fail_links;

	// [2nd] NOTE:
	// struct ctl_table 의 46 개 크기만큼 할당 받은 메모리 kmem_cache#24-oX 에
	// kern_table의 child 멤버 값이 NULL 인 index 의 table 값을 복사한 값

	// [1st][f1] header->ctl_table: (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table),
	// [1st][f1] entry: (kmem_cache#29-oX + 52) (struct ctl_table), ((kmem_cache#29-oX + 52) (struct ctl_table))->procname: "kernel"
	// [2nd][f1] header->ctl_table: (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
	// [2nd][f1] entry: kmem_cache#24-oX (struct ctl_table), (kmem_cache#24-oX (struct ctl_table))->procname: "sched_child_runs_first"
	for (entry = header->ctl_table; entry->procname; entry++) {
		// [1st][f2] entry: ((kmem_cache#29-oX + 52) (struct ctl_table))[1]
		// [1st][f2] entry->procname: (((kmem_cache#29-oX + 52) (struct ctl_table))[1]).procname: NULL

		// [2nd][f2] entry: (kmem_cache#24-oX (struct ctl_table))[1]
		// [2nd][f2] entry->procname: ((kmem_cache#24-oX (struct ctl_table))[1]).procname: "sched_min_granularity_ns"

		// [1st][f1] header: &(kmem_cache#29-oX)->header, entry: (kmem_cache#29-oX + 52) (struct ctl_table)
		// [1st][f1] insert_entry(&(kmem_cache#29-oX)->header, (kmem_cache#29-oX + 52) (struct ctl_table)): 0
		// [2nd][f1] header: kmem_cache#25-oX, entry: kmem_cache#24-oX (struct ctl_table)
		// [2nd][f1] insert_entry(kmem_cache#25-oX, kmem_cache#24-oX (struct ctl_table)): 0
		// [2nd][f2] header: kmem_cache#25-oX, entry: (kmem_cache#24-oX (struct ctl_table))[1]
		// [2nd][f2] insert_entry(kmem_cache#25-oX, kmem_cache#24-oX (struct ctl_table)): 0
		err = insert_entry(header, entry);
		// [1st][f1] err: 0
		// [2nd][f1] err: 0
		// [2nd][f2] err: 0

		// [1st][f1] insert_entry 에서 한일:
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
		// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
		//
		// &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
		// (proc의 kernel directory)
		/*
		//                          proc-b
		//                         (kernel)
		*/

		// [2nd][f1] insert_entry 에서 한일:
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
		// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		//
		// &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node 을 black node 로 추가
		// (kern_table 의 1 번째 index의 값)
		/*
		//                       kern_table-b
		//                 (sched_child_runs_first)
		*/

		// [2nd][f2] insert_entry 에서 한일:
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
		//
		// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
		// (kern_table 의 2 번째 index의 값)
		/*
		//                       kern_table-b
		//                 (sched_child_runs_first)
		//                                      \
		//                                        kern_table-r
		//                                  (sched_min_granularity_ns)
		//
		*/

		// [1st][f1] err: 0
		// [2nd][f1] err: 0
		// [2nd][f2] err: 0
		if (err)
			goto fail;

		// [2nd][f2] 위의 loop를 kern_table 의 index 수만큼 수행 (46개)
	}

	// [2nd] 위의 loop 수행 결과
	// TODO: kern_table 의 rbtree 그림을 그려야함

// 2016/07/09 종료
// 2016/07/16 시작

	return 0;
	// [1st] return 0
	// [2nd] return 0
fail:
	erase_header(header);
	put_links(header);
fail_links:
	header->parent = NULL;
	drop_sysctl_table(&dir->header);
	return err;
}

/* called under sysctl_lock */
static int use_table(struct ctl_table_header *p)
{
	if (unlikely(p->unregistering))
		return 0;
	p->used++;
	return 1;
}

/* called under sysctl_lock */
static void unuse_table(struct ctl_table_header *p)
{
	if (!--p->used)
		if (unlikely(p->unregistering))
			complete(p->unregistering);
}

/* called under sysctl_lock, will reacquire if has to wait */
static void start_unregistering(struct ctl_table_header *p)
{
	/*
	 * if p->used is 0, nobody will ever touch that entry again;
	 * we'll eliminate all paths to it before dropping sysctl_lock
	 */
	if (unlikely(p->used)) {
		struct completion wait;
		init_completion(&wait);
		p->unregistering = &wait;
		spin_unlock(&sysctl_lock);
		wait_for_completion(&wait);
		spin_lock(&sysctl_lock);
	} else {
		/* anything non-NULL; we'll never dereference it */
		p->unregistering = ERR_PTR(-EINVAL);
	}
	/*
	 * do not remove from the list until nobody holds it; walking the
	 * list in do_sysctl() relies on that.
	 */
	erase_header(p);
}

static void sysctl_head_get(struct ctl_table_header *head)
{
	spin_lock(&sysctl_lock);
	head->count++;
	spin_unlock(&sysctl_lock);
}

void sysctl_head_put(struct ctl_table_header *head)
{
	spin_lock(&sysctl_lock);
	if (!--head->count)
		kfree_rcu(head, rcu);
	spin_unlock(&sysctl_lock);
}

static struct ctl_table_header *sysctl_head_grab(struct ctl_table_header *head)
{
	BUG_ON(!head);
	spin_lock(&sysctl_lock);
	if (!use_table(head))
		head = ERR_PTR(-ENOENT);
	spin_unlock(&sysctl_lock);
	return head;
}

static void sysctl_head_finish(struct ctl_table_header *head)
{
	if (!head)
		return;
	spin_lock(&sysctl_lock);
	unuse_table(head);
	spin_unlock(&sysctl_lock);
}

static struct ctl_table_set *
lookup_header_set(struct ctl_table_root *root, struct nsproxy *namespaces)
{
	struct ctl_table_set *set = &root->default_set;
	if (root->lookup)
		set = root->lookup(root, namespaces);
	return set;
}

static struct ctl_table *lookup_entry(struct ctl_table_header **phead,
				      struct ctl_dir *dir,
				      const char *name, int namelen)
{
	struct ctl_table_header *head;
	struct ctl_table *entry;

	spin_lock(&sysctl_lock);
	entry = find_entry(&head, dir, name, namelen);
	if (entry && use_table(head))
		*phead = head;
	else
		entry = NULL;
	spin_unlock(&sysctl_lock);
	return entry;
}

static struct ctl_node *first_usable_entry(struct rb_node *node)
{
	struct ctl_node *ctl_node;

	for (;node; node = rb_next(node)) {
		ctl_node = rb_entry(node, struct ctl_node, node);
		if (use_table(ctl_node->header))
			return ctl_node;
	}
	return NULL;
}

static void first_entry(struct ctl_dir *dir,
	struct ctl_table_header **phead, struct ctl_table **pentry)
{
	struct ctl_table_header *head = NULL;
	struct ctl_table *entry = NULL;
	struct ctl_node *ctl_node;

	spin_lock(&sysctl_lock);
	ctl_node = first_usable_entry(rb_first(&dir->root));
	spin_unlock(&sysctl_lock);
	if (ctl_node) {
		head = ctl_node->header;
		entry = &head->ctl_table[ctl_node - head->node];
	}
	*phead = head;
	*pentry = entry;
}

static void next_entry(struct ctl_table_header **phead, struct ctl_table **pentry)
{
	struct ctl_table_header *head = *phead;
	struct ctl_table *entry = *pentry;
	struct ctl_node *ctl_node = &head->node[entry - head->ctl_table];

	spin_lock(&sysctl_lock);
	unuse_table(head);

	ctl_node = first_usable_entry(rb_next(&ctl_node->node));
	spin_unlock(&sysctl_lock);
	head = NULL;
	if (ctl_node) {
		head = ctl_node->header;
		entry = &head->ctl_table[ctl_node - head->node];
	}
	*phead = head;
	*pentry = entry;
}

void register_sysctl_root(struct ctl_table_root *root)
{
}

/*
 * sysctl_perm does NOT grant the superuser all rights automatically, because
 * some sysctl variables are readonly even to root.
 */

static int test_perm(int mode, int op)
{
	if (uid_eq(current_euid(), GLOBAL_ROOT_UID))
		mode >>= 6;
	else if (in_egroup_p(GLOBAL_ROOT_GID))
		mode >>= 3;
	if ((op & ~mode & (MAY_READ|MAY_WRITE|MAY_EXEC)) == 0)
		return 0;
	return -EACCES;
}

static int sysctl_perm(struct ctl_table_header *head, struct ctl_table *table, int op)
{
	struct ctl_table_root *root = head->root;
	int mode;

	if (root->permissions)
		mode = root->permissions(head, table);
	else
		mode = table->mode;

	return test_perm(mode, op);
}

static struct inode *proc_sys_make_inode(struct super_block *sb,
		struct ctl_table_header *head, struct ctl_table *table)
{
	struct inode *inode;
	struct proc_inode *ei;

	inode = new_inode(sb);
	if (!inode)
		goto out;

	inode->i_ino = get_next_ino();

	sysctl_head_get(head);
	ei = PROC_I(inode);
	ei->sysctl = head;
	ei->sysctl_entry = table;

	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_mode = table->mode;
	if (!S_ISDIR(table->mode)) {
		inode->i_mode |= S_IFREG;
		inode->i_op = &proc_sys_inode_operations;
		inode->i_fop = &proc_sys_file_operations;
	} else {
		inode->i_mode |= S_IFDIR;
		inode->i_op = &proc_sys_dir_operations;
		inode->i_fop = &proc_sys_dir_file_operations;
	}
out:
	return inode;
}

static struct ctl_table_header *grab_header(struct inode *inode)
{
	struct ctl_table_header *head = PROC_I(inode)->sysctl;
	if (!head)
		head = &sysctl_table_root.default_set.dir.header;
	return sysctl_head_grab(head);
}

static struct dentry *proc_sys_lookup(struct inode *dir, struct dentry *dentry,
					unsigned int flags)
{
	struct ctl_table_header *head = grab_header(dir);
	struct ctl_table_header *h = NULL;
	struct qstr *name = &dentry->d_name;
	struct ctl_table *p;
	struct inode *inode;
	struct dentry *err = ERR_PTR(-ENOENT);
	struct ctl_dir *ctl_dir;
	int ret;

	if (IS_ERR(head))
		return ERR_CAST(head);

	ctl_dir = container_of(head, struct ctl_dir, header);

	p = lookup_entry(&h, ctl_dir, name->name, name->len);
	if (!p)
		goto out;

	if (S_ISLNK(p->mode)) {
		ret = sysctl_follow_link(&h, &p, current->nsproxy);
		err = ERR_PTR(ret);
		if (ret)
			goto out;
	}

	err = ERR_PTR(-ENOMEM);
	inode = proc_sys_make_inode(dir->i_sb, h ? h : head, p);
	if (!inode)
		goto out;

	err = NULL;
	d_set_d_op(dentry, &proc_sys_dentry_operations);
	d_add(dentry, inode);

out:
	if (h)
		sysctl_head_finish(h);
	sysctl_head_finish(head);
	return err;
}

static ssize_t proc_sys_call_handler(struct file *filp, void __user *buf,
		size_t count, loff_t *ppos, int write)
{
	struct inode *inode = file_inode(filp);
	struct ctl_table_header *head = grab_header(inode);
	struct ctl_table *table = PROC_I(inode)->sysctl_entry;
	ssize_t error;
	size_t res;

	if (IS_ERR(head))
		return PTR_ERR(head);

	/*
	 * At this point we know that the sysctl was not unregistered
	 * and won't be until we finish.
	 */
	error = -EPERM;
	if (sysctl_perm(head, table, write ? MAY_WRITE : MAY_READ))
		goto out;

	/* if that can happen at all, it should be -EINVAL, not -EISDIR */
	error = -EINVAL;
	if (!table->proc_handler)
		goto out;

	/* careful: calling conventions are nasty here */
	res = count;
	error = table->proc_handler(table, write, buf, &res, ppos);
	if (!error)
		error = res;
out:
	sysctl_head_finish(head);

	return error;
}

static ssize_t proc_sys_read(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	return proc_sys_call_handler(filp, (void __user *)buf, count, ppos, 0);
}

static ssize_t proc_sys_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *ppos)
{
	return proc_sys_call_handler(filp, (void __user *)buf, count, ppos, 1);
}

static int proc_sys_open(struct inode *inode, struct file *filp)
{
	struct ctl_table_header *head = grab_header(inode);
	struct ctl_table *table = PROC_I(inode)->sysctl_entry;

	/* sysctl was unregistered */
	if (IS_ERR(head))
		return PTR_ERR(head);

	if (table->poll)
		filp->private_data = proc_sys_poll_event(table->poll);

	sysctl_head_finish(head);

	return 0;
}

static unsigned int proc_sys_poll(struct file *filp, poll_table *wait)
{
	struct inode *inode = file_inode(filp);
	struct ctl_table_header *head = grab_header(inode);
	struct ctl_table *table = PROC_I(inode)->sysctl_entry;
	unsigned int ret = DEFAULT_POLLMASK;
	unsigned long event;

	/* sysctl was unregistered */
	if (IS_ERR(head))
		return POLLERR | POLLHUP;

	if (!table->proc_handler)
		goto out;

	if (!table->poll)
		goto out;

	event = (unsigned long)filp->private_data;
	poll_wait(filp, &table->poll->wait, wait);

	if (event != atomic_read(&table->poll->event)) {
		filp->private_data = proc_sys_poll_event(table->poll);
		ret = POLLIN | POLLRDNORM | POLLERR | POLLPRI;
	}

out:
	sysctl_head_finish(head);

	return ret;
}

static bool proc_sys_fill_cache(struct file *file,
				struct dir_context *ctx,
				struct ctl_table_header *head,
				struct ctl_table *table)
{
	struct dentry *child, *dir = file->f_path.dentry;
	struct inode *inode;
	struct qstr qname;
	ino_t ino = 0;
	unsigned type = DT_UNKNOWN;

	qname.name = table->procname;
	qname.len  = strlen(table->procname);
	qname.hash = full_name_hash(qname.name, qname.len);

	child = d_lookup(dir, &qname);
	if (!child) {
		child = d_alloc(dir, &qname);
		if (child) {
			inode = proc_sys_make_inode(dir->d_sb, head, table);
			if (!inode) {
				dput(child);
				return false;
			} else {
				d_set_d_op(child, &proc_sys_dentry_operations);
				d_add(child, inode);
			}
		} else {
			return false;
		}
	}
	inode = child->d_inode;
	ino  = inode->i_ino;
	type = inode->i_mode >> 12;
	dput(child);
	return dir_emit(ctx, qname.name, qname.len, ino, type);
}

static bool proc_sys_link_fill_cache(struct file *file,
				    struct dir_context *ctx,
				    struct ctl_table_header *head,
				    struct ctl_table *table)
{
	bool ret = true;
	head = sysctl_head_grab(head);

	if (S_ISLNK(table->mode)) {
		/* It is not an error if we can not follow the link ignore it */
		int err = sysctl_follow_link(&head, &table, current->nsproxy);
		if (err)
			goto out;
	}

	ret = proc_sys_fill_cache(file, ctx, head, table);
out:
	sysctl_head_finish(head);
	return ret;
}

static int scan(struct ctl_table_header *head, ctl_table *table,
		unsigned long *pos, struct file *file,
		struct dir_context *ctx)
{
	bool res;

	if ((*pos)++ < ctx->pos)
		return true;

	if (unlikely(S_ISLNK(table->mode)))
		res = proc_sys_link_fill_cache(file, ctx, head, table);
	else
		res = proc_sys_fill_cache(file, ctx, head, table);

	if (res)
		ctx->pos = *pos;

	return res;
}

static int proc_sys_readdir(struct file *file, struct dir_context *ctx)
{
	struct ctl_table_header *head = grab_header(file_inode(file));
	struct ctl_table_header *h = NULL;
	struct ctl_table *entry;
	struct ctl_dir *ctl_dir;
	unsigned long pos;

	if (IS_ERR(head))
		return PTR_ERR(head);

	ctl_dir = container_of(head, struct ctl_dir, header);

	if (!dir_emit_dots(file, ctx))
		return 0;

	pos = 2;

	for (first_entry(ctl_dir, &h, &entry); h; next_entry(&h, &entry)) {
		if (!scan(h, entry, &pos, file, ctx)) {
			sysctl_head_finish(h);
			break;
		}
	}
	sysctl_head_finish(head);
	return 0;
}

static int proc_sys_permission(struct inode *inode, int mask)
{
	/*
	 * sysctl entries that are not writeable,
	 * are _NOT_ writeable, capabilities or not.
	 */
	struct ctl_table_header *head;
	struct ctl_table *table;
	int error;

	/* Executable files are not allowed under /proc/sys/ */
	if ((mask & MAY_EXEC) && S_ISREG(inode->i_mode))
		return -EACCES;

	head = grab_header(inode);
	if (IS_ERR(head))
		return PTR_ERR(head);

	table = PROC_I(inode)->sysctl_entry;
	if (!table) /* global root - r-xr-xr-x */
		error = mask & MAY_WRITE ? -EACCES : 0;
	else /* Use the permissions on the sysctl table entry */
		error = sysctl_perm(head, table, mask & ~MAY_NOT_BLOCK);

	sysctl_head_finish(head);
	return error;
}

static int proc_sys_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	int error;

	if (attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID))
		return -EPERM;

	error = inode_change_ok(inode, attr);
	if (error)
		return error;

	setattr_copy(inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

static int proc_sys_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct ctl_table_header *head = grab_header(inode);
	struct ctl_table *table = PROC_I(inode)->sysctl_entry;

	if (IS_ERR(head))
		return PTR_ERR(head);

	generic_fillattr(inode, stat);
	if (table)
		stat->mode = (stat->mode & S_IFMT) | table->mode;

	sysctl_head_finish(head);
	return 0;
}

static const struct file_operations proc_sys_file_operations = {
	.open		= proc_sys_open,
	.poll		= proc_sys_poll,
	.read		= proc_sys_read,
	.write		= proc_sys_write,
	.llseek		= default_llseek,
};

// ARM10C 20160611
static const struct file_operations proc_sys_dir_file_operations = {
	.read		= generic_read_dir,
	.iterate	= proc_sys_readdir,
	.llseek		= generic_file_llseek,
};

static const struct inode_operations proc_sys_inode_operations = {
	.permission	= proc_sys_permission,
	.setattr	= proc_sys_setattr,
	.getattr	= proc_sys_getattr,
};

// ARM10C 20160611
static const struct inode_operations proc_sys_dir_operations = {
	.lookup		= proc_sys_lookup,
	.permission	= proc_sys_permission,
	.setattr	= proc_sys_setattr,
	.getattr	= proc_sys_getattr,
};

static int proc_sys_revalidate(struct dentry *dentry, unsigned int flags)
{
	if (flags & LOOKUP_RCU)
		return -ECHILD;
	return !PROC_I(dentry->d_inode)->sysctl->unregistering;
}

static int proc_sys_delete(const struct dentry *dentry)
{
	return !!PROC_I(dentry->d_inode)->sysctl->unregistering;
}

static int sysctl_is_seen(struct ctl_table_header *p)
{
	struct ctl_table_set *set = p->set;
	int res;
	spin_lock(&sysctl_lock);
	if (p->unregistering)
		res = 0;
	else if (!set->is_seen)
		res = 1;
	else
		res = set->is_seen(set);
	spin_unlock(&sysctl_lock);
	return res;
}

static int proc_sys_compare(const struct dentry *parent, const struct dentry *dentry,
		unsigned int len, const char *str, const struct qstr *name)
{
	struct ctl_table_header *head;
	struct inode *inode;

	/* Although proc doesn't have negative dentries, rcu-walk means
	 * that inode here can be NULL */
	/* AV: can it, indeed? */
	inode = ACCESS_ONCE(dentry->d_inode);
	if (!inode)
		return 1;
	if (name->len != len)
		return 1;
	if (memcmp(name->name, str, len))
		return 1;
	head = rcu_dereference(PROC_I(inode)->sysctl);
	return !head || !sysctl_is_seen(head);
}

static const struct dentry_operations proc_sys_dentry_operations = {
	.d_revalidate	= proc_sys_revalidate,
	.d_delete	= proc_sys_delete,
	.d_compare	= proc_sys_compare,
};

// ARM10C 20160702
// dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
// ARM10C 20160702
// dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
static struct ctl_dir *find_subdir(struct ctl_dir *dir,
				   const char *name, int namelen)
{
	struct ctl_table_header *head;
	struct ctl_table *entry;

	// dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
	// find_entry(&head, &(&sysctl_table_root.default_set)->dir, "kernel/", 6): NULL
	entry = find_entry(&head, dir, name, namelen);
	// entry: NULL

	// entry: NULL
	if (!entry)
		// ENOENT: 2, ERR_PTR(-2): 0xfffffffe
		return ERR_PTR(-ENOENT);
		// return 0xfffffffe

	if (!S_ISDIR(entry->mode))
		return ERR_PTR(-ENOTDIR);
	return container_of(head, struct ctl_dir, header);
}

// ARM10C 20160702
// set: &sysctl_table_root.default_set, name: kmem_cache#23-oX: "kernel/", namelen: 6
static struct ctl_dir *new_dir(struct ctl_table_set *set,
			       const char *name, int namelen)
{
	struct ctl_table *table;
	struct ctl_dir *new;
	struct ctl_node *node;
	char *new_name;

	// sizeof(struct ctl_dir): 36 bytes, sizeof(struct ctl_node): 16 bytes,
	// sizeof(struct ctl_table): 34 bytes, namelen: 6, GFP_KERNEL: 0xD0
	// kzalloc(127, GFP_KERNEL: 0xD0): kmem_cache#29-oX
	new = kzalloc(sizeof(*new) + sizeof(struct ctl_node) +
		      sizeof(struct ctl_table)*2 +  namelen + 1,
		      GFP_KERNEL);
	// new: kmem_cache#29-oX

	// new: kmem_cache#29-oX
	if (!new)
		return NULL;

	// new: kmem_cache#29-oX
	node = (struct ctl_node *)(new + 1);
	// node: (kmem_cache#29-oX + 36) (struct ctl_node)

	// node: (kmem_cache#29-oX + 36) (struct ctl_node)
	table = (struct ctl_table *)(node + 1);
	// table: (kmem_cache#29-oX + 52) (struct ctl_table)

	// table: (kmem_cache#29-oX + 52) (struct ctl_table)
	new_name = (char *)(table + 2);
	// new_name: (kmem_cache#29-oX + 120) (char)

	// new_name: (kmem_cache#29-oX + 120) (char), name: kmem_cache#23-oX: "kernel/", namelen: 6
	memcpy(new_name, name, namelen);

	// memcpy 에서 한일:
	// new_name: (kmem_cache#29-oX + 120): "kernel"

	// new_name: (kmem_cache#29-oX + 120): "kernel", namelen: 6
	new_name[namelen] = '\0';
	// new_name: (kmem_cache#29-oX + 120): "kernel"

	// table[0].procname: ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname, new_name: (kmem_cache#29-oX + 120): "kernel"
	table[0].procname = new_name;
	// table[0].procname: ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"

	// table[0].mode: ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode, S_IFDIR: 0040000, S_IRUGO: 00444, S_IXUGO: 00111
	table[0].mode = S_IFDIR|S_IRUGO|S_IXUGO;
	// table[0].mode: ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555

	// &new->header: &(kmem_cache#29-oX)->header, set->dir.header.root: (&sysctl_table_root.default_set)->dir.header.root,
	// set: &sysctl_table_root.default_set, node: (kmem_cache#29-oX + 36) (struct ctl_node), table: (kmem_cache#29-oX + 52) (struct ctl_table)
	init_header(&new->header, set->dir.header.root, set, node, table);

	// init_header 에서 한일:
	// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->used: 0
	// (&(kmem_cache#29-oX)->header)->count: 1
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	// (&(kmem_cache#29-oX)->header)->unregistering: NULL
	// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
	// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
	// (&(kmem_cache#29-oX)->header)->parent: NULL
	// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
	// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header

	// new: kmem_cache#29-oX
	return new;
	// return kmem_cache#29-oX
}

/**
 * get_subdir - find or create a subdir with the specified name.
 * @dir:  Directory to create the subdirectory in
 * @name: The name of the subdirectory to find or create
 * @namelen: The length of name
 *
 * Takes a directory with an elevated reference count so we know that
 * if we drop the lock the directory will not go away.  Upon success
 * the reference is moved from @dir to the returned subdirectory.
 * Upon error an error code is returned and the reference on @dir is
 * simply dropped.
 */
// ARM10C 20160702
// dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
static struct ctl_dir *get_subdir(struct ctl_dir *dir,
				  const char *name, int namelen)
{
	// dir->header.set: (&(&sysctl_table_root.default_set)->dir)->header.set: &sysctl_table_root.default_set
	struct ctl_table_set *set = dir->header.set;
	// set: &sysctl_table_root.default_set

	struct ctl_dir *subdir, *new = NULL;
	// new: NULL

	int err;

	spin_lock(&sysctl_lock);

	// spin_lock에서 한일:
	// &sysctl_lock을 이용한 spin lock 수행

	// dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
	// find_subdir(&(&sysctl_table_root.default_set)->dir, "kernel/", 6): 0xfffffffe
	subdir = find_subdir(dir, name, namelen);
	// subdir: 0xfffffffe

	// subdir: 0xfffffffe, IS_ERR(0xfffffffe): 1
	if (!IS_ERR(subdir))
		goto found;

	// subdir: 0xfffffffe, PTR_ERR(0xfffffffe): -2, ENOENT: 2
	if (PTR_ERR(subdir) != -ENOENT)
		goto failed;

	spin_unlock(&sysctl_lock);

	// spin_unlock에서 한일:
	// &sysctl_lock을 이용한 spin unlock 수행

	// set: &sysctl_table_root.default_set, name: kmem_cache#23-oX: "kernel/", namelen: 6
	// new_dir(&sysctl_table_root.default_set, "kernel/", 6): kmem_cache#29-oX
	new = new_dir(set, name, namelen);
	// new: kmem_cache#29-oX

	// new_dir 에서 한일:
	// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
	// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
	//
	// (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
	// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->used: 0
	// (&(kmem_cache#29-oX)->header)->count: 1
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	// (&(kmem_cache#29-oX)->header)->unregistering: NULL
	// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
	// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
	// (&(kmem_cache#29-oX)->header)->parent: NULL
	// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
	// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header

	spin_lock(&sysctl_lock);

	// spin_lock에서 한일:
	// &sysctl_lock을 이용한 spin lock 수행

	// subdir: 0xfffffffe, ENOMEM: 12, ERR_PTR(-12): 0xfffffff4
	subdir = ERR_PTR(-ENOMEM);
	// subdir: 0xfffffff4

	// new: kmem_cache#29-oX
	if (!new)
		goto failed;

	/* Was the subdir added while we dropped the lock? */
	// dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
	// find_subdir(&(&sysctl_table_root.default_set)->dir, "kernel/", 6): 0xfffffffe
	subdir = find_subdir(dir, name, namelen);
	// subdir: 0xfffffffe

	// subdir: 0xfffffffe, IS_ERR(0xfffffffe): 1
	if (!IS_ERR(subdir))
		goto found;

	// subdir: 0xfffffffe, PTR_ERR(0xfffffffe): -2, ENOENT: 2
	if (PTR_ERR(subdir) != -ENOENT)
		goto failed;

	/* Nope.  Use the our freshly made directory entry. */
	// dir: &(&sysctl_table_root.default_set)->dir, &new->header: &(kmem_cache#29-oX)->header
	// insert_header(&(&sysctl_table_root.default_set)->dir, &(kmem_cache#29-oX)->header): 0
	err = insert_header(dir, &new->header);
	// err: 0

	// insert_header 에서 한일:
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
	// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	//
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
	// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
	//
	// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
	/*
	//                          proc-b
	//                         (kernel)
	*/

	// err: 0, ERR_PTR(0): 0
	subdir = ERR_PTR(err);
	// subdir: 0

	// err: 0
	if (err)
		goto failed;

	// subdir: 0, new: kmem_cache#29-oX
	subdir = new;
	// subdir: kmem_cache#29-oX
found:
	// subdir->header.nreg: (kmem_cache#29-oX)->header.nreg: 1
	subdir->header.nreg++;
	// subdir->header.nreg: (kmem_cache#29-oX)->header.nreg: 2
failed:
	// subdir: kmem_cache#29-oX, IS_ERR(kmem_cache#29-oX): 0
	if (unlikely(IS_ERR(subdir))) {
		pr_err("sysctl could not get directory: ");
		sysctl_print_dir(dir);
		pr_cont("/%*.*s %ld\n",
			namelen, namelen, name, PTR_ERR(subdir));
	}

	// &dir->header: &(&(&sysctl_table_root.default_set)->dir)->header
	drop_sysctl_table(&dir->header);

	// drop_sysctl_table 에서 한일:
	// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2

	// new: kmem_cache#29-oX
	if (new)
		// &new->header: &(kmem_cache#29-oX)->header
		drop_sysctl_table(&new->header);

		// drop_sysctl_table 에서 한일:
		// (&(kmem_cache#29-oX)->header)->nreg: 1

	spin_unlock(&sysctl_lock);

	// spin_unlock에서 한일:
	// &sysctl_lock을 이용한 spin unlock 수행

	// subdir: kmem_cache#29-oX
	return subdir;
	// return kmem_cache#29-oX
}

static struct ctl_dir *xlate_dir(struct ctl_table_set *set, struct ctl_dir *dir)
{
	struct ctl_dir *parent;
	const char *procname;
	if (!dir->header.parent)
		return &set->dir;
	parent = xlate_dir(set, dir->header.parent);
	if (IS_ERR(parent))
		return parent;
	procname = dir->header.ctl_table[0].procname;
	return find_subdir(parent, procname, strlen(procname));
}

static int sysctl_follow_link(struct ctl_table_header **phead,
	struct ctl_table **pentry, struct nsproxy *namespaces)
{
	struct ctl_table_header *head;
	struct ctl_table_root *root;
	struct ctl_table_set *set;
	struct ctl_table *entry;
	struct ctl_dir *dir;
	int ret;

	ret = 0;
	spin_lock(&sysctl_lock);
	root = (*pentry)->data;
	set = lookup_header_set(root, namespaces);
	dir = xlate_dir(set, (*phead)->parent);
	if (IS_ERR(dir))
		ret = PTR_ERR(dir);
	else {
		const char *procname = (*pentry)->procname;
		head = NULL;
		entry = find_entry(&head, dir, procname, strlen(procname));
		ret = -ENOENT;
		if (entry && use_table(head)) {
			unuse_table(*phead);
			*phead = head;
			*pentry = entry;
			ret = 0;
		}
	}

	spin_unlock(&sysctl_lock);
	return ret;
}

static int sysctl_err(const char *path, struct ctl_table *table, char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	pr_err("sysctl table check failed: %s/%s %pV\n",
	       path, table->procname, &vaf);

	va_end(args);
	return -EINVAL;
}

// ARM10C 20160702
// path: kmem_cache#23-oX, table: kmem_cache#24-oX
static int sysctl_check_table(const char *path, struct ctl_table *table)
{
	int err = 0;
	// err: 0

	// table->procname: (kmem_cache#24-oX)->procname: "sched_child_runs_first"
	for (; table->procname; table++) {
		// table->child: (kmem_cache#24-oX)->child: NULL
		if (table->child)
			err = sysctl_err(path, table, "Not a file");

		// table->proc_handler: (kmem_cache#24-oX)->proc_handler: proc_dointvec
		if ((table->proc_handler == proc_dostring) ||
		    (table->proc_handler == proc_dointvec) ||
		    (table->proc_handler == proc_dointvec_minmax) ||
		    (table->proc_handler == proc_dointvec_jiffies) ||
		    (table->proc_handler == proc_dointvec_userhz_jiffies) ||
		    (table->proc_handler == proc_dointvec_ms_jiffies) ||
		    (table->proc_handler == proc_doulongvec_minmax) ||
		    (table->proc_handler == proc_doulongvec_ms_jiffies_minmax)) {
			// table->data: (kmem_cache#24-oX)->data: &sysctl_sched_child_runs_first
			if (!table->data)
				err = sysctl_err(path, table, "No data");

			// table->maxlen: (kmem_cache#24-oX)->maxlen: 4
			if (!table->maxlen)
				err = sysctl_err(path, table, "No maxlen");
		}

		// table->proc_handler: (kmem_cache#24-oX)->proc_handler: proc_dointvec
		if (!table->proc_handler)
			err = sysctl_err(path, table, "No proc_handler");

		// table->mode: (kmem_cache#24-oX)->mode: 0644, S_IRUGO: 00444, S_IWUGO: 00222
		if ((table->mode & (S_IRUGO|S_IWUGO)) != table->mode)
			err = sysctl_err(path, table, "bogus .mode 0%o",
				table->mode);

		// table: kmem_cache#24-oX (kern_table) 의 child 없는 맴버 46개 만큼 위 loop를 수행
	}

	// err: 0
	return err;
	// return 0
}

static struct ctl_table_header *new_links(struct ctl_dir *dir, struct ctl_table *table,
	struct ctl_table_root *link_root)
{
	struct ctl_table *link_table, *entry, *link;
	struct ctl_table_header *links;
	struct ctl_node *node;
	char *link_name;
	int nr_entries, name_bytes;

	name_bytes = 0;
	nr_entries = 0;
	for (entry = table; entry->procname; entry++) {
		nr_entries++;
		name_bytes += strlen(entry->procname) + 1;
	}

	links = kzalloc(sizeof(struct ctl_table_header) +
			sizeof(struct ctl_node)*nr_entries +
			sizeof(struct ctl_table)*(nr_entries + 1) +
			name_bytes,
			GFP_KERNEL);

	if (!links)
		return NULL;

	node = (struct ctl_node *)(links + 1);
	link_table = (struct ctl_table *)(node + nr_entries);
	link_name = (char *)&link_table[nr_entries + 1];

	for (link = link_table, entry = table; entry->procname; link++, entry++) {
		int len = strlen(entry->procname) + 1;
		memcpy(link_name, entry->procname, len);
		link->procname = link_name;
		link->mode = S_IFLNK|S_IRWXUGO;
		link->data = link_root;
		link_name += len;
	}
	init_header(links, dir->header.root, dir->header.set, node, link_table);
	links->nreg = nr_entries;

	return links;
}

static bool get_links(struct ctl_dir *dir,
	struct ctl_table *table, struct ctl_table_root *link_root)
{
	struct ctl_table_header *head;
	struct ctl_table *entry, *link;

	/* Are there links available for every entry in table? */
	for (entry = table; entry->procname; entry++) {
		const char *procname = entry->procname;
		link = find_entry(&head, dir, procname, strlen(procname));
		if (!link)
			return false;
		if (S_ISDIR(link->mode) && S_ISDIR(entry->mode))
			continue;
		if (S_ISLNK(link->mode) && (link->data == link_root))
			continue;
		return false;
	}

	/* The checks passed.  Increase the registration count on the links */
	for (entry = table; entry->procname; entry++) {
		const char *procname = entry->procname;
		link = find_entry(&head, dir, procname, strlen(procname));
		head->nreg++;
	}
	return true;
}

// ARM10C 20160702
// header: &(kmem_cache#29-oX)->header
// ARM10C 20160709
// header: kmem_cache#25-oX
static int insert_links(struct ctl_table_header *head)
{
	struct ctl_table_set *root_set = &sysctl_table_root.default_set;
	// root_set: &sysctl_table_root.default_set
	// root_set: &sysctl_table_root.default_set

	struct ctl_dir *core_parent = NULL;
	// core_parent: NULL
	// core_parent: NULL

	struct ctl_table_header *links;
	int err;

	// head->set: (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set,
	// root_set: &sysctl_table_root.default_set
	// head->set: (kmem_cache#25-oX)->set: &sysctl_table_root.default_set,
	// root_set: &sysctl_table_root.default_set
	if (head->set == root_set)
		return 0;
		// return 0
		// return 0

	core_parent = xlate_dir(root_set, head->parent);
	if (IS_ERR(core_parent))
		return 0;

	if (get_links(core_parent, head->ctl_table, head->root))
		return 0;

	core_parent->header.nreg++;
	spin_unlock(&sysctl_lock);

	links = new_links(core_parent, head->ctl_table, head->root);

	spin_lock(&sysctl_lock);
	err = -ENOMEM;
	if (!links)
		goto out;

	err = 0;
	if (get_links(core_parent, head->ctl_table, head->root)) {
		kfree(links);
		goto out;
	}

	err = insert_header(core_parent, links);
	if (err)
		kfree(links);
out:
	drop_sysctl_table(&core_parent->header);
	return err;
}

/**
 * __register_sysctl_table - register a leaf sysctl table
 * @set: Sysctl tree to register on
 * @path: The path to the directory the sysctl table is in.
 * @table: the top-level table structure
 *
 * Register a sysctl table hierarchy. @table should be a filled in ctl_table
 * array. A completely 0 filled entry terminates the table.
 *
 * The members of the &struct ctl_table structure are used as follows:
 *
 * procname - the name of the sysctl file under /proc/sys. Set to %NULL to not
 *            enter a sysctl file
 *
 * data - a pointer to data for use by proc_handler
 *
 * maxlen - the maximum size in bytes of the data
 *
 * mode - the file permissions for the /proc/sys file
 *
 * child - must be %NULL.
 *
 * proc_handler - the text handler routine (described below)
 *
 * extra1, extra2 - extra pointers usable by the proc handler routines
 *
 * Leaf nodes in the sysctl tree will be represented by a single file
 * under /proc; non-leaf nodes will be represented by directories.
 *
 * There must be a proc_handler routine for any terminal nodes.
 * Several default handlers are available to cover common cases -
 *
 * proc_dostring(), proc_dointvec(), proc_dointvec_jiffies(),
 * proc_dointvec_userhz_jiffies(), proc_dointvec_minmax(),
 * proc_doulongvec_ms_jiffies_minmax(), proc_doulongvec_minmax()
 *
 * It is the handler's job to read the input buffer from user memory
 * and process it. The handler should return 0 on success.
 *
 * This routine returns %NULL on a failure to register, and a pointer
 * to the table header on success.
 */
// ARM10C 20160625
// [rc1] set: &sysctl_table_root.default_set, path: kmem_cache#23-oX, files: kmem_cache#24-oX
struct ctl_table_header *__register_sysctl_table(
	struct ctl_table_set *set,
	const char *path, struct ctl_table *table)
{
	// set->dir.header.root: (&sysctl_table_root.default_set)->dir.header.root: &sysctl_table_root
	struct ctl_table_root *root = set->dir.header.root;
	// root: &sysctl_table_root

	struct ctl_table_header *header;
	const char *name, *nextname;
	struct ctl_dir *dir;
	struct ctl_table *entry;
	struct ctl_node *node;
	int nr_entries = 0;
	// nr_entries: 0

	// table: kmem_cache#24-oX, entry: kmem_cache#24-oX, entry->procname: (kmem_cache#24-oX)->procname: "sched_child_runs_first"
	for (entry = table; entry->procname; entry++)
		// nr_entries: 0
		nr_entries++;
		// nr_entries: 1

		// kern_table 의 child 없는 index 만큼 loop 수행

	// 위 loop 의 수행결과:
	// nr_entries: 46

	// sizeof(struct ctl_table_header): 32 bytes, sizeof(struct ctl_node): 16 bytes, nr_entries: 46, GFP_KERNEL: 0xD0
	// kzalloc(768, GFP_KERNEL: 0xD0): kmem_cache#25-oX
	header = kzalloc(sizeof(struct ctl_table_header) +
			 sizeof(struct ctl_node)*nr_entries, GFP_KERNEL);
	// header: kmem_cache#25-oX

	// header: kmem_cache#25-oX
	if (!header)
		return NULL;

	// header: kmem_cache#25-oX
	node = (struct ctl_node *)(header + 1);
	// node: &(kmem_cache#25-oX)[1] (struct ctl_node)

// 2016/06/25 종료
// 2016/07/02 시작

	// header: kmem_cache#25-oX, root: &sysctl_table_root, set: &sysctl_table_root.default_set,
	// node: &(kmem_cache#25-oX)[1] (struct ctl_node), table: kmem_cache#24-oX
	init_header(header, root, set, node, table);

	// init_header 에서 한일:
	// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#25-oX)->used: 0
	// (kmem_cache#25-oX)->count: 1
	// (kmem_cache#25-oX)->nreg: 1
	// (kmem_cache#25-oX)->unregistering: NULL
	// (kmem_cache#25-oX)->root: &sysctl_table_root
	// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
	// (kmem_cache#25-oX)->parent: NULL
	// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
	// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX

	// path: kmem_cache#23-oX, table: kmem_cache#24-oX
	// sysctl_check_table(kmem_cache#23-oX, kmem_cache#24-oX): 0
	if (sysctl_check_table(path, table))
		goto fail;

	// sysctl_check_table 에서 한일:
	// table: kmem_cache#24-oX (kern_table) 의 child 없는 맴버 46개 만큼 loop를 수행하면서
	// kern_table 의 맴버값을 체크함

	spin_lock(&sysctl_lock);

	// spin_lock에서 한일:
	// &sysctl_lock을 이용한 spin lock 수행

	// &set->dir: &(&sysctl_table_root.default_set)->dir
	dir = &set->dir;
	// dir: &(&sysctl_table_root.default_set)->dir

	/* Reference moved down the diretory tree get_subdir */
	// dir->header.nreg: (&(&sysctl_table_root.default_set)->dir)->header.nreg: 1
	dir->header.nreg++;
	// dir->header.nreg: (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2

	spin_unlock(&sysctl_lock);

	// spin_unlock에서 한일:
	// &sysctl_lock을 이용한 spin unlock 수행

	/* Find the directory for the ctl_table */
	// [f1] path: kmem_cache#23-oX: "kernel/", name: kmem_cache#23-oX: "kernel/"
	for (name = path; name; name = nextname) {
		int namelen;

		// [f2] name: kmem_cache#23-oX: "kernel/" 의 '/' 위치의 주소값+1: NULL

		// [f1] name: kmem_cache#23-oX: "kernel/"
		// [f1] strchr(kmem_cache#23-oX, '/'): kmem_cache#23-oX 의 '/' 위치의 주소값
		nextname = strchr(name, '/');
		// [f1] nextname: kmem_cache#23-oX: "kernel/" 의 '/' 위치의 주소값

		// [f1] nextname: kmem_cache#23-oX: "kernel/" 의 '/' 위치의 주소값
		if (nextname) {
			// [f1] nextname: kmem_cache#23-oX: "kernel/" 의 '/' 위치의 주소값,
			// [f1] name: kmem_cache#23-oX: "kernel/"
			namelen = nextname - name;
			// [f1] namelen: 6

			// [f1] nextname: kmem_cache#23-oX: "kernel/" 의 '/' 위치의 주소값
			nextname++;
			// [f1] nextname: kmem_cache#23-oX: "kernel/" 의 '/' 위치의 주소값+1
		} else {
			namelen = strlen(name);
		}

		// [f1] namelen: 6
		if (namelen == 0)
			continue;

		// [f1] dir: &(&sysctl_table_root.default_set)->dir, name: kmem_cache#23-oX: "kernel/", namelen: 6
		// [f1] get_subdir(&(&sysctl_table_root.default_set)->dir, "kernel/", 6): kmem_cache#29-oX
		dir = get_subdir(dir, name, namelen);
		// [f1] dir: kmem_cache#29-oX

		// [f1] get_subdir 에서 한일:
		// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
		// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
		//
		// (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
		// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->used: 0
		// (&(kmem_cache#29-oX)->header)->count: 1
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		// (&(kmem_cache#29-oX)->header)->unregistering: NULL
		// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
		// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
		// (&(kmem_cache#29-oX)->header)->parent: NULL
		// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
		// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
		// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
		//
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
		// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
		//
		// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
		/*
		//                          proc-b
		//                         (kernel)
		*/
		// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
		// (&(kmem_cache#29-oX)->header)->nreg: 1

		// [f1] dir: kmem_cache#29-oX, IS_ERR(kmem_cache#29-oX): 0
		if (IS_ERR(dir))
			goto fail;

// 2016/07/02 종료
// 2016/07/09 시작

	}

	spin_lock(&sysctl_lock);

	// spin_unlock에서 한일:
	// &sysctl_lock을 이용한 spin unlock 수행

	// dir: kmem_cache#29-oX, header: kmem_cache#25-oX
	// insert_header(kmem_cache#29-oX, kmem_cache#25-oX): 0
	if (insert_header(dir, header))
		goto fail_put_dir_locked;

	// insert_header 에서 한일:
	// (kmem_cache#29-oX)->header.nreg: 2
	// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	//
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
	// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
	//
	// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
	// (kern_table 의 2 번째 index의 값)
	/*
	//                       kern_table-b
	//                 (sched_child_runs_first)
	//                                      \
	//                                        kern_table-r
	//                                  (sched_min_granularity_ns)
	//
	// ..... kern_table 의 index 수만큼 RB Tree를 구성
	// 아래 링크의 RB Tree 그림 참고
	// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
	//
	// TODO: kern_table 의 rbtree 그림을 그려야함
	*/

	// &dir->header: &(kmem_cache#29-oX)->header
	drop_sysctl_table(&dir->header);

	spin_unlock(&sysctl_lock);

	// spin_unlock에서 한일:
	// &sysctl_lock을 이용한 spin unlock 수행

	// header: kmem_cache#25-oX
	return header;
	// return kmem_cache#25-oX

fail_put_dir_locked:
	drop_sysctl_table(&dir->header);
	spin_unlock(&sysctl_lock);
fail:
	kfree(header);
	dump_stack();
	return NULL;
}

/**
 * register_sysctl - register a sysctl table
 * @path: The path to the directory the sysctl table is in.
 * @table: the table structure
 *
 * Register a sysctl table. @table should be a filled in ctl_table
 * array. A completely 0 filled entry terminates the table.
 *
 * See __register_sysctl_table for more details.
 */
struct ctl_table_header *register_sysctl(const char *path, struct ctl_table *table)
{
	return __register_sysctl_table(&sysctl_table_root.default_set,
					path, table);
}
EXPORT_SYMBOL(register_sysctl);

// ARM10C 20160625
// path: kmem_cache#23-oX, pos: kmem_cache#23-oX,
// entry->procname: sysctl_base_table[0].procname: "kernel"
static char *append_path(const char *path, char *pos, const char *name)
{
	int namelen;

	// name: "kernel", strlen("kernel"): 6
	namelen = strlen(name);
	// namelen: 6

	// pos: kmem_cache#23-oX, path: kmem_cache#23-oX, namelen: 6, PATH_MAX: 4096
	if (((pos - path) + namelen + 2) >= PATH_MAX)
		return NULL;

	// pos: kmem_cache#23-oX, name: "kernel", namelen: 6
	memcpy(pos, name, namelen);

	// memcpy 에서 한일:
	// pos: kmem_cache#23-oX: "kernel"

	// namelen: 6, pos[6]: (kmem_cache#23-oX)[6]: NULL
	pos[namelen] = '/';
	// pos[6]: (kmem_cache#23-oX)[6]: '/'

	// namelen: 6, pos[7]: (kmem_cache#23-oX)[7]
	pos[namelen + 1] = '\0';
	// namelen: 6, pos[7]: (kmem_cache#23-oX)[7]: '\0'

	// pos: kmem_cache#23-oX: "kernel/", namelen: 6
	pos += namelen + 1;
	// pos: &(kmem_cache#23-oX)[7]

	// pos: &(kmem_cache#23-oX)[7]
	return pos;
	// return &(kmem_cache#23-oX)[7]
}

// ARM10C 20160611
// table: sysctl_base_table
static int count_subheaders(struct ctl_table *table)
{
	int has_files = 0;
	// has_files: 0

	int nr_subheaders = 0;
	// nr_subheaders: 0

	struct ctl_table *entry;

	/* special case: no directory and empty directory */
	// table: sysctl_base_table, table->procname: sysctl_base_table[0].procname: "kernel"
	if (!table || !table->procname)
		return 1;

// 2016/06/11 종료
// 2016/06/25 시작

	// table: sysctl_base_table, entry: sysctl_base_table, entry->procname: sysctl_base_table[0].procname: "kernel"
	for (entry = table; entry->procname; entry++) {

		// [f1] entry->child: sysctl_base_table[0].child: kern_table
		// [f2] entry->child: sysctl_base_table[1].child: vm_table
		// [f3] entry->child: sysctl_base_table[2].child: fs_table
		// [f4] entry->child: sysctl_base_table[3].child: debug_table
		// [f5] entry->child: sysctl_base_table[4].child: dev_table
		if (entry->child)
			// [f1] nr_subheaders: 0, entry->child: sysctl_base_table[0].child: kern_table
			// [f1] count_subheadera(kern_table): 2
			// [f2] nr_subheaders: 2, entry->child: sysctl_base_table[1].child: vm_table
			// [f2] count_subheadera(vm_table): 0
			// [f3] nr_subheaders: 2, entry->child: sysctl_base_table[2].child: fs_table
			// [f3] count_subheadera(fs_table): 0
			// [f4] nr_subheaders: 2, entry->child: sysctl_base_table[3].child: debug_table
			// [f4] count_subheadera(debug_table): 0
			// [f5] nr_subheaders: 2, entry->child: sysctl_base_table[4].child: dev_table
			// [f5] count_subheadera(dev_table): 0
			nr_subheaders += count_subheaders(entry->child);
			// [f1] nr_subheaders: 2
			// [f2] nr_subheaders: 2
			// [f3] nr_subheaders: 2
			// [f4] nr_subheaders: 2
			// [f5] nr_subheaders: 2
		else
			has_files = 1;
	}

	// nr_subheaders: 2, has_files: 0
	return nr_subheaders + has_files;
	// return 2
}

// ARM10C 20160625
// new_path: kmem_cache#23-oX, pos: kmem_cache#23-oX, subheader: &(kmem_cache#27-oX)[1] (struct ctl_table_header)
// set: &sysctl_table_root.default_set, table: sysctl_base_table
// ARM10C 20160625
// [rc1] path: kmem_cache#23-oX, child_pos: &(kmem_cache#23-oX)[7] subheader: &&(kmem_cache#27-oX)[1] (struct ctl_table_header)
// [rc1] set: &sysctl_table_root.default_set, entry->child: sysctl_base_table[0].child: kern_table
static int register_leaf_sysctl_tables(const char *path, char *pos,
	struct ctl_table_header ***subheader, struct ctl_table_set *set,
	struct ctl_table *table)
{
	struct ctl_table *ctl_table_arg = NULL;
	// ctl_table_arg: NULL
	// [rc1] ctl_table_arg: NULL

	struct ctl_table *entry, *files;
	int nr_files = 0;
	// nr_files: 0
	// [rc1] nr_files: 0

	int nr_dirs = 0;
	// nr_dirs: 0
	// [rc1] nr_dirs: 0

	// ENOMEM: 12
	// [rc1] ENOMEM: 12
	int err = -ENOMEM;
	// err: -12
	// [rc1] err: -12

	// table: sysctl_base_table, entry: sysctl_base_table,
	// entry->procname: sysctl_base_table[0].procname: "kernel"
	// [rc1] table: kern_table, entry: kern_table,
	// [rc1] entry->procname: kern_table[0].procname: "sched_child_runs_first"
	for (entry = table; entry->procname; entry++) {
		// entry->child: sysctl_base_table[0].child: kern_table
		// [rc1] entry->child: kern_table[0].child: NULL
		if (entry->child)
			// nr_dirs: 0
			nr_dirs++;
			// nr_dirs: 1
		else
			// [rc1] nr_files: 0
			nr_files++;
			// [rc1] nr_files: 1

		// sysctl_base_table 맴버 값 만큼 loop 수행
		// [rc1] kern_table 맴버 값 만큼 loop 수행
	}

	// 위 loop를 수행한 결과
	// nr_dirs: 5

	// [rc1] 위 loop를 수행한 결과
	// [rc1] nr_dirs: 2, [rc1] nr_files: 46

	// table: sysctl_base_table
	// [rc1] table: kern_table
	files = table;
	// files: sysctl_base_table
	// [rc1] files: kern_table

	/* If there are mixed files and directories we need a new table */
	// nr_dirs: 5, nr_files: 0
	// [rc1] nr_dirs: 2, nr_files: 46
	if (nr_dirs && nr_files) {
		struct ctl_table *new;

		// [rc1] sizeof(struct ctl_table): 34 bytes, nr_files: 46, GFP_KERNEL: 0xD0
		// [rc1] kzalloc(1598, GFP_KERNEL: 0xD0): kmem_cache#24-oX
		files = kzalloc(sizeof(struct ctl_table) * (nr_files + 1),
				GFP_KERNEL);
		// [rc1] files: kmem_cache#24-oX

		// [rc1] files: kmem_cache#24-oX
		if (!files)
			goto out;

		// [rc1] ctl_table_arg: NULL, [rc1] files: kmem_cache#24-oX
		ctl_table_arg = files;
		// [rc1] ctl_table_arg: kmem_cache#24-oX

		// [rc1] files: kmem_cache#24-oX, new: kmem_cache#24-oX, table: kern_table, entry: kern_table,
		// [rc1] entry->procname: kern_table[0].procname: "sched_child_runs_first"
		for (new = files, entry = table; entry->procname; entry++) {
			// [rc1] entry->child: kern_table[0].child: NULL
			if (entry->child)
				continue;
			// [rc1] *new: (kmem_cache#24-oX)[0], *entry: kern_table[0]
			*new = *entry;
			// [rc1] *new: (kmem_cache#24-oX)[0]: kern_table[0]

			// [rc1] new: kmem_cache#24-oX
			new++;
			// [rc1] new: &(kmem_cache#24-oX + 1)

			// [rc1] kern_table 이 가지고 있는 index 만큼 loop 수행
		}

		// [rc1] 위 loop의 수행 결과:
		// struct ctl_table 의 46 개 크기만큼 할당 받은 메모리 kmem_cache#24-oX 에
		// kern_table의 child 멤버 값이 NULL 인 index 의 table 값을 복사함
	}

	/* Register everything except a directory full of subdirectories */
	// nr_files: 0, nr_dirs: 5
	// [rc1] nr_files: 46, nr_dirs: 2
	if (nr_files || !nr_dirs) {
		struct ctl_table_header *header;

		// [rc1] set: &sysctl_table_root.default_set, path: kmem_cache#23-oX, files: kmem_cache#24-oX
		// __register_sysctl_table(&sysctl_table_root.default_set, kmem_cache#23-oX, kmem_cache#24-oX): kmem_cache#25-oX
		header = __register_sysctl_table(set, path, files);
		// [rc1] header: kmem_cache#25-oX

		// [rc1] __register_sysctl_table 에서 한일:
		// struct ctl_table_header 의 메모리 + struct ctl_node 의 메모리 46 개를 할당 받음 kmem_cache#25-oX
		//
		// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
		// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
		// (kmem_cache#25-oX)->used: 0
		// (kmem_cache#25-oX)->count: 1
		// (kmem_cache#25-oX)->nreg: 1
		// (kmem_cache#25-oX)->unregistering: NULL
		// (kmem_cache#25-oX)->root: &sysctl_table_root
		// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
		// (kmem_cache#25-oX)->parent: NULL
		// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
		// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
		//
		// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
		// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
		//
		// (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
		// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->used: 0
		// (&(kmem_cache#29-oX)->header)->count: 1
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		// (&(kmem_cache#29-oX)->header)->unregistering: NULL
		// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
		// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
		// (&(kmem_cache#29-oX)->header)->parent: NULL
		// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
		// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
		// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
		//
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
		// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
		//
		// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
		/*
		//                          proc-b
		//                         (kernel)
		*/
		// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		//
		// (kmem_cache#29-oX)->header.nreg: 2
		// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
		//
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
		// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
		//
		// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
		// (kern_table 의 2 번째 index의 값)
		/*
		//                       kern_table-b
		//                 (sched_child_runs_first)
		//                                      \
		//                                        kern_table-r
		//                                  (sched_min_granularity_ns)
		//
		// ..... kern_table 의 index 수만큼 RB Tree를 구성
		// 아래 링크의 RB Tree 그림 참고
		// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
		//
		// TODO: kern_table 의 rbtree 그림을 그려야함
		*/

		// [rc1] header: kmem_cache#25-oX
		if (!header) {
			kfree(ctl_table_arg);
			goto out;
		}

		/* Remember if we need to free the file table */
		// [rc1] header->ctl_table_arg: (kmem_cache#25-oX)->ctl_table_arg, ctl_table_arg: kmem_cache#24-oX
		header->ctl_table_arg = ctl_table_arg;
		// [rc1] header->ctl_table_arg: (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX

		// [rc1] **subheader: (kmem_cache#27-oX)[1] (struct ctl_table_header), header: kmem_cache#25-oX
		**subheader = header;
		// [rc1] (kmem_cache#27-oX)[1] (struct ctl_table_header): kmem_cache#25-oX

		// [rc1] *subheader: &(kmem_cache#27-oX)[1] (struct ctl_table_header)
		(*subheader)++;
		// [rc1] *subheader: &(kmem_cache#27-oX)[2] (struct ctl_table_header)
	}

	/* Recurse into the subdirectories. */
	// table: sysctl_base_table, entry: sysctl_base_table,
	// entry->procname: sysctl_base_table[0].procname: "kernel"
	// [rc1] table: kern_table, entry: kern_table,
	// [rc1] entry->procname: kern_table[0].procname: "sched_child_runs_first"
	for (entry = table; entry->procname; entry++) {
		char *child_pos;

		// entry->child: sysctl_base_table[0].child: kern_table
		// [rc1] entry->child: kern_table[0].child: NULL
		if (!entry->child)
			continue;
			// [rc1] continue 수행

		// ENAMETOOLONG: 36
		err = -ENAMETOOLONG;
		// err: -36

		// path: kmem_cache#23-oX, pos: kmem_cache#23-oX,
		// entry->procname: sysctl_base_table[0].procname: "kernel"
		// append_path(kmem_cache#23-oX, kmem_cache#23-oX, "kernel"): &(kmem_cache#23-oX)[7]
		child_pos = append_path(path, pos, entry->procname);
		// child_pos: &(kmem_cache#23-oX)[7]

		// append_path 에서 한일:
		// pos: kmem_cache#23-oX: "kernel/"

		// child_pos: &(kmem_cache#23-oX)[7]
		if (!child_pos)
			goto out;

		// path: kmem_cache#23-oX, child_pos: &(kmem_cache#23-oX)[7] subheader: &(kmem_cache#27-oX)[1] (struct ctl_table_header)
		// set: &sysctl_table_root.default_set, entry->child: sysctl_base_table[0].child: kern_table
		// register_leaf_sysctl_tables(kmem_cache#23-oX, &(kmem_cache#23-oX)[7], &(kmem_cache#27-oX)[1] (struct ctl_table_header), 
		//                             &sysctl_table_root.default_set, kern_table): 0
		err = register_leaf_sysctl_tables(path, child_pos, subheader,
						  set, entry->child);
		// err: 0

		// register_leaf_sysctl_tables 에서 한일:
		// struct ctl_table_header 의 메모리 + struct ctl_node 의 메모리 46 개를 할당 받음 kmem_cache#25-oX
		//
		// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
		// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
		// (kmem_cache#25-oX)->used: 0
		// (kmem_cache#25-oX)->count: 1
		// (kmem_cache#25-oX)->nreg: 1
		// (kmem_cache#25-oX)->unregistering: NULL
		// (kmem_cache#25-oX)->root: &sysctl_table_root
		// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
		// (kmem_cache#25-oX)->parent: NULL
		// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
		// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
		//
		// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
		// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
		//
		// (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
		// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->used: 0
		// (&(kmem_cache#29-oX)->header)->count: 1
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		// (&(kmem_cache#29-oX)->header)->unregistering: NULL
		// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
		// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
		// (&(kmem_cache#29-oX)->header)->parent: NULL
		// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
		// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
		// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
		//
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
		// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
		//
		// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
		/*
		//                          proc-b
		//                         (kernel)
		*/
		// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		//
		// (kmem_cache#29-oX)->header.nreg: 2
		// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
		//
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
		// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
		//
		// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
		// (kern_table 의 2 번째 index의 값)
		/*
		//                       kern_table-b
		//                 (sched_child_runs_first)
		//                                      \
		//                                        kern_table-r
		//                                  (sched_min_granularity_ns)
		//
		// ..... kern_table 의 index 수만큼 RB Tree를 구성
		// 아래 링크의 RB Tree 그림 참고
		// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
		//
		// TODO: kern_table 의 rbtree 그림을 그려야함
		*/
		// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
		// (kmem_cache#27-oX)[1] (struct ctl_table_header): kmem_cache#25-oX

		pos[0] = '\0';
		if (err)
			goto out;

		// [rc1] kern_table index 만큼 loop 수행
		// sysctl_base_table index 만큼 loop 수행
	}

	// [rc1] 위 loop 에서 수행 한일:
	// kern_table index 만큼 kern_table[...].child 맴버값을 보고 dir, files 의 RB Tree 를 구성함

	// 위 loop 에서 수행 한일:
	// sysctl_base_table index 만큼 sysctl_base_table[...].child 맴버값을 보고 dir, files 의 RB Tree 를 구성함

	err = 0;
	// [rc1] err: 0
	// err: 0
out:
	/* On failure our caller will unregister all registered subheaders */
	// [rc1] err: 0
	// err: 0
	return err;
	// [rc1] return 0
	// return 0
}

/**
 * __register_sysctl_paths - register a sysctl table hierarchy
 * @set: Sysctl tree to register on
 * @path: The path to the directory the sysctl table is in.
 * @table: the top-level table structure
 *
 * Register a sysctl table hierarchy. @table should be a filled in ctl_table
 * array. A completely 0 filled entry terminates the table.
 *
 * See __register_sysctl_table for more details.
 */
// ARM10C 20160611
// &sysctl_table_root.default_set, path: null_path, table: sysctl_base_table
struct ctl_table_header *__register_sysctl_paths(
	struct ctl_table_set *set,
	const struct ctl_path *path, struct ctl_table *table)
{
	// table: sysctl_base_table
	struct ctl_table *ctl_table_arg = table;
	// ctl_table_arg: sysctl_base_table

	// table: sysctl_base_table, count_subheaders(sysctl_base_table): 2
	int nr_subheaders = count_subheaders(table);
	// nr_subheaders: 2

	struct ctl_table_header *header = NULL, **subheaders, **subheader;
	// header: NULL

	const struct ctl_path *component;
	char *new_path, *pos;

	// PATH_MAX: 4096, GFP_KERNEL: 0xD0,
	// kmalloc(4096, GFP_KERNEL: 0xD0): kmem_cache#23-oX
	pos = new_path = kmalloc(PATH_MAX, GFP_KERNEL);
	// pos: kmem_cache#23-oX, new_path: kmem_cache#23-oX

	// new_path: kmem_cache#23-oX
	if (!new_path)
		return NULL;

	// pos[0]: (kmem_cache#23-oX)[0]
	pos[0] = '\0';
	// pos[0]: (kmem_cache#23-oX)[0]: '\0'

	// path: null_path, component: null_path, component->procname: null_path->procname: NULL
	for (component = path; component->procname; component++) {
		pos = append_path(new_path, pos, component->procname);
		if (!pos)
			goto out;
	}

	// table->procname: sysctl_base_table[0].procname: "kernel",
	// table->child: sysctl_base_table[0].child: kern_table,
	// table[1]->procname: sysctl_base_table[1].procname: "vm",
	while (table->procname && table->child && !table[1].procname) {
		pos = append_path(new_path, pos, table->procname);
		if (!pos)
			goto out;
		table = table->child;
	}

	// nr_subheaders: 2
	if (nr_subheaders == 1) {
		header = __register_sysctl_table(set, new_path, table);
		if (header)
			header->ctl_table_arg = ctl_table_arg;
	} else {
		// header: NULL, sizeof(struct ctl_table_header): 32 bytes
		// sizeof(*header): 32, sizeof(*subheaders): 4, nr_subheaders: 2, GFP_KERNEL: 0xD0,
		// kzalloc(256, GFP_KERNEL: 0xD0): kmem_cache#27-oX
		header = kzalloc(sizeof(*header) +
				 sizeof(*subheaders)*nr_subheaders, GFP_KERNEL);
		// header: kmem_cache#27-oX

		// header: kmem_cache#27-oX
		if (!header)
			goto out;

		// header: kmem_cache#27-oX
		subheaders = (struct ctl_table_header **) (header + 1);
		// subheaders: &(kmem_cache#27-oX)[1] (struct ctl_table_header)

		// subheaders: &(kmem_cache#27-oX)[1] (struct ctl_table_header)
		subheader = subheaders;
		// subheader: &(kmem_cache#27-oX)[1] (struct ctl_table_header)

		// header->ctl_table_arg: (kmem_cache#27-oX)->ctl_table_arg, ctl_table_arg: sysctl_base_table
		header->ctl_table_arg = ctl_table_arg;
		// header->ctl_table_arg: (kmem_cache#27-oX)->ctl_table_arg: sysctl_base_table

		// new_path: kmem_cache#23-oX, pos: kmem_cache#23-oX, subheader: &(kmem_cache#27-oX)[1] (struct ctl_table_header)
		// set: &sysctl_table_root.default_set, table: sysctl_base_table
		// register_leaf_sysctl_tables(kmem_cache#23-oX, kmem_cache#23-oX, &&(kmem_cache#27-oX)[1] (struct ctl_table_header)
		//                             &sysctl_table_root.default_set, sysctl_base_table): 0
		if (register_leaf_sysctl_tables(new_path, pos, &subheader,
						set, table))
			goto err_register_leaves;

		// register_leaf_sysctl_tables 에서 한일:
		// struct ctl_table_header 의 메모리 + struct ctl_node 의 메모리 46 개를 할당 받음 kmem_cache#25-oX
		//
		// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
		// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
		// (kmem_cache#25-oX)->used: 0
		// (kmem_cache#25-oX)->count: 1
		// (kmem_cache#25-oX)->nreg: 1
		// (kmem_cache#25-oX)->unregistering: NULL
		// (kmem_cache#25-oX)->root: &sysctl_table_root
		// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
		// (kmem_cache#25-oX)->parent: NULL
		// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
		// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
		//
		// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
		// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
		//
		// (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
		// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
		// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
		// (&(kmem_cache#29-oX)->header)->used: 0
		// (&(kmem_cache#29-oX)->header)->count: 1
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		// (&(kmem_cache#29-oX)->header)->unregistering: NULL
		// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
		// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
		// (&(kmem_cache#29-oX)->header)->parent: NULL
		// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
		// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
		//
		// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
		// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
		//
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
		// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
		// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
		//
		// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
		/*
		//                          proc-b
		//                         (kernel)
		*/
		// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
		// (&(kmem_cache#29-oX)->header)->nreg: 1
		//
		// (kmem_cache#29-oX)->header.nreg: 2
		// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
		//
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
		// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
		// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
		//
		// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
		// (kern_table 의 2 번째 index의 값)
		/*
		//                       kern_table-b
		//                 (sched_child_runs_first)
		//                                      \
		//                                        kern_table-r
		//                                  (sched_min_granularity_ns)
		//
		// ..... kern_table 의 index 수만큼 RB Tree를 구성
		// 아래 링크의 RB Tree 그림 참고
		// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
		//
		// TODO: kern_table 의 rbtree 그림을 그려야함
		*/
		// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
		// (kmem_cache#27-oX)[1] (struct ctl_table_header): kmem_cache#25-oX
		//
		// kern_table index 만큼 kern_table[...].child 맴버값을 보고 dir, files 의 RB Tree 를 구성함
		//
		// sysctl_base_table index 만큼 sysctl_base_table[...].child 맴버값을 보고 recursive 하게 dir, files 의 RB Tree 를 구성함
	}

out:
	// new_path: kmem_cache#23-oX
	kfree(new_path);

	// kfree 에서 한일:
	// kmem_cache#23-oX 를 다른 용도로 사용할 수 있도록 kmem_cache 에게 돌려줌

	// header: kmem_cache#27-oX
	return header;
	// return kmem_cache#27-oX

err_register_leaves:
	while (subheader > subheaders) {
		struct ctl_table_header *subh = *(--subheader);
		struct ctl_table *table = subh->ctl_table_arg;
		unregister_sysctl_table(subh);
		kfree(table);
	}
	kfree(header);
	header = NULL;
	goto out;
}

/**
 * register_sysctl_table_path - register a sysctl table hierarchy
 * @path: The path to the directory the sysctl table is in.
 * @table: the top-level table structure
 *
 * Register a sysctl table hierarchy. @table should be a filled in ctl_table
 * array. A completely 0 filled entry terminates the table.
 *
 * See __register_sysctl_paths for more details.
 */
// ARM10C 20160611
// null_path, table: sysctl_base_table
struct ctl_table_header *register_sysctl_paths(const struct ctl_path *path,
						struct ctl_table *table)
{
	// path: null_path, table: sysctl_base_table
	// __register_sysctl_paths(&sysctl_table_root.default_set, null_path, sysctl_base_table): kmem_cache#27-oX
	return __register_sysctl_paths(&sysctl_table_root.default_set,
					path, table);
	// return kmem_cache#27-oX

	// __register_sysctl_paths 에서 한일:
	// struct ctl_table_header 의 메모리 +  struct ctl_table_header 의 포인터 메모리 * 2 를 할당 받음 kmem_cache#27-oX
	//
	// struct ctl_table_header 의 메모리 + struct ctl_node 의 메모리 46 개를 할당 받음 kmem_cache#25-oX
	//
	// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#25-oX)->used: 0
	// (kmem_cache#25-oX)->count: 1
	// (kmem_cache#25-oX)->nreg: 1
	// (kmem_cache#25-oX)->unregistering: NULL
	// (kmem_cache#25-oX)->root: &sysctl_table_root
	// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
	// (kmem_cache#25-oX)->parent: NULL
	// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
	// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX
	//
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
	//
	// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
	// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
	//
	// (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
	// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->used: 0
	// (&(kmem_cache#29-oX)->header)->count: 1
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	// (&(kmem_cache#29-oX)->header)->unregistering: NULL
	// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
	// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
	// (&(kmem_cache#29-oX)->header)->parent: NULL
	// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
	// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
	//
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
	// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	//
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
	// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
	//
	// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
	/*
	//                          proc-b
	//                         (kernel)
	*/
	// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	//
	// (kmem_cache#29-oX)->header.nreg: 2
	// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	//
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
	// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
	//
	// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
	// (kern_table 의 2 번째 index의 값)
	/*
	//                       kern_table-b
	//                 (sched_child_runs_first)
	//                                      \
	//                                        kern_table-r
	//                                  (sched_min_granularity_ns)
	//
	// ..... kern_table 의 index 수만큼 RB Tree를 구성
	// 아래 링크의 RB Tree 그림 참고
	// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
	//
	// TODO: kern_table 의 rbtree 그림을 그려야함
	*/
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#27-oX)[1] (struct ctl_table_header): kmem_cache#25-oX
	//
	// kern_table index 만큼 kern_table[...].child 맴버값을 보고 dir, files 의 RB Tree 를 구성함
	//
	// sysctl_base_table index 만큼 sysctl_base_table[...].child 맴버값을 보고 recursive 하게 dir, files 의 RB Tree 를 구성함
}
EXPORT_SYMBOL(register_sysctl_paths);

/**
 * register_sysctl_table - register a sysctl table hierarchy
 * @table: the top-level table structure
 *
 * Register a sysctl table hierarchy. @table should be a filled in ctl_table
 * array. A completely 0 filled entry terminates the table.
 *
 * See register_sysctl_paths for more details.
 */
// ARM10C 20160611
// sysctl_base_table
struct ctl_table_header *register_sysctl_table(struct ctl_table *table)
{
	static const struct ctl_path null_path[] = { {} };

	// table: sysctl_base_table
	// register_sysctl_paths(null_path, sysctl_base_table): kmem_cache#27-oX
	return register_sysctl_paths(null_path, table);
	// return kmem_cache#27-oX

	// register_sysctl_paths 에서 한일:
	// struct ctl_table_header 의 메모리 +  struct ctl_table_header 의 포인터 메모리 * 2 를 할당 받음 kmem_cache#27-oX
	//
	// struct ctl_table_header 의 메모리 + struct ctl_node 의 메모리 46 개를 할당 받음 kmem_cache#25-oX
	//
	// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#25-oX)->used: 0
	// (kmem_cache#25-oX)->count: 1
	// (kmem_cache#25-oX)->nreg: 1
	// (kmem_cache#25-oX)->unregistering: NULL
	// (kmem_cache#25-oX)->root: &sysctl_table_root
	// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
	// (kmem_cache#25-oX)->parent: NULL
	// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
	// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX
	//
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
	//
	// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
	// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
	//
	// (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
	// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->used: 0
	// (&(kmem_cache#29-oX)->header)->count: 1
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	// (&(kmem_cache#29-oX)->header)->unregistering: NULL
	// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
	// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
	// (&(kmem_cache#29-oX)->header)->parent: NULL
	// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
	// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
	//
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
	// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	//
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
	// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
	//
	// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
	/*
	//                          proc-b
	//                         (kernel)
	*/
	// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	//
	// (kmem_cache#29-oX)->header.nreg: 2
	// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	//
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
	// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
	//
	// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
	// (kern_table 의 2 번째 index의 값)
	/*
	//                       kern_table-b
	//                 (sched_child_runs_first)
	//                                      \
	//                                        kern_table-r
	//                                  (sched_min_granularity_ns)
	//
	// ..... kern_table 의 index 수만큼 RB Tree를 구성
	// 아래 링크의 RB Tree 그림 참고
	// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
	//
	// TODO: kern_table 의 rbtree 그림을 그려야함
	*/
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#27-oX)[1] (struct ctl_table_header): kmem_cache#25-oX
	//
	// kern_table index 만큼 kern_table[...].child 맴버값을 보고 dir, files 의 RB Tree 를 구성함
	//
	// sysctl_base_table index 만큼 sysctl_base_table[...].child 맴버값을 보고 recursive 하게 dir, files 의 RB Tree 를 구성함
}
EXPORT_SYMBOL(register_sysctl_table);

static void put_links(struct ctl_table_header *header)
{
	struct ctl_table_set *root_set = &sysctl_table_root.default_set;
	struct ctl_table_root *root = header->root;
	struct ctl_dir *parent = header->parent;
	struct ctl_dir *core_parent;
	struct ctl_table *entry;

	if (header->set == root_set)
		return;

	core_parent = xlate_dir(root_set, parent);
	if (IS_ERR(core_parent))
		return;

	for (entry = header->ctl_table; entry->procname; entry++) {
		struct ctl_table_header *link_head;
		struct ctl_table *link;
		const char *name = entry->procname;

		link = find_entry(&link_head, core_parent, name, strlen(name));
		if (link &&
		    ((S_ISDIR(link->mode) && S_ISDIR(entry->mode)) ||
		     (S_ISLNK(link->mode) && (link->data == root)))) {
			drop_sysctl_table(link_head);
		}
		else {
			pr_err("sysctl link missing during unregister: ");
			sysctl_print_dir(parent);
			pr_cont("/%s\n", name);
		}
	}
}

// ARM10C 20160702
// &dir->header: &(&(&sysctl_table_root.default_set)->dir)->header
// ARM10C 20160702
// &new->header: &(kmem_cache#29-oX)->header
// ARM10C 20160716
// &dir->header: &(kmem_cache#29-oX)->header
static void drop_sysctl_table(struct ctl_table_header *header)
{
	// header->parent: (&(&(&sysctl_table_root.default_set)->dir)->header)->parent: NULL
	// header->parent: (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	// header->parent: (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	struct ctl_dir *parent = header->parent;
	// parent: NULL
	// parent: &(&sysctl_table_root.default_set)->dir
	// parent: &(&sysctl_table_root.default_set)->dir

	// header->nreg: (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 3
	// header->nreg: (&(kmem_cache#29-oX)->header)->nreg: 2
	// header->nreg: (&(kmem_cache#29-oX)->header)->nreg: 2
	if (--header->nreg)
		// header->nreg: (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
		// header->nreg: (&(kmem_cache#29-oX)->header)->nreg: 1
		// header->nreg: (&(kmem_cache#29-oX)->header)->nreg: 1
		return;
		// return
		// return
		// return

	put_links(header);
	start_unregistering(header);
	if (!--header->count)
		kfree_rcu(header, rcu);

	if (parent)
		drop_sysctl_table(&parent->header);
}

/**
 * unregister_sysctl_table - unregister a sysctl table hierarchy
 * @header: the header returned from register_sysctl_table
 *
 * Unregisters the sysctl table and all children. proc entries may not
 * actually be removed until they are no longer used by anyone.
 */
void unregister_sysctl_table(struct ctl_table_header * header)
{
	int nr_subheaders;
	might_sleep();

	if (header == NULL)
		return;

	nr_subheaders = count_subheaders(header->ctl_table_arg);
	if (unlikely(nr_subheaders > 1)) {
		struct ctl_table_header **subheaders;
		int i;

		subheaders = (struct ctl_table_header **)(header + 1);
		for (i = nr_subheaders -1; i >= 0; i--) {
			struct ctl_table_header *subh = subheaders[i];
			struct ctl_table *table = subh->ctl_table_arg;
			unregister_sysctl_table(subh);
			kfree(table);
		}
		kfree(header);
		return;
	}

	spin_lock(&sysctl_lock);
	drop_sysctl_table(header);
	spin_unlock(&sysctl_lock);
}
EXPORT_SYMBOL(unregister_sysctl_table);

void setup_sysctl_set(struct ctl_table_set *set,
	struct ctl_table_root *root,
	int (*is_seen)(struct ctl_table_set *))
{
	memset(set, 0, sizeof(*set));
	set->is_seen = is_seen;
	init_header(&set->dir.header, root, set, NULL, root_table);
}

void retire_sysctl_set(struct ctl_table_set *set)
{
	WARN_ON(!RB_EMPTY_ROOT(&set->dir.root));
}

// ARM10C 20160611
int __init proc_sys_init(void)
{
	struct proc_dir_entry *proc_sys_root;

	// proc_mkdir("sys", NULL): kmem_cache#29-oX (struct proc_dir_entry)
	proc_sys_root = proc_mkdir("sys", NULL);
	// proc_sys_root: kmem_cache#29-oX (struct proc_dir_entry)

	// proc_mkdir 에서 한일:
	// struct proc_dir_entry 만큼 메모리를 할당 받음 kmem_cache#29-oX (struct proc_dir_entry)
	//
	// (kmem_cache#29-oX (struct proc_dir_entry))->name: "sys"
	// (kmem_cache#29-oX (struct proc_dir_entry))->namelen: 3
	// (kmem_cache#29-oX (struct proc_dir_entry))->mode: 0040555
	// (kmem_cache#29-oX (struct proc_dir_entry))->nlink: 2
	// (&(kmem_cache#29-oX (struct proc_dir_entry))->count)->counter: 1
	// &(kmem_cache#29-oX (struct proc_dir_entry))->pde_unload_lock을 이용한 spin lock 초기화 수행
	// ((&(kmem_cache#29-oX (struct proc_dir_entry))->pde_unload_lock)->rlock)->raw_lock: { { 0 } }
	// ((&(kmem_cache#29-oX (struct proc_dir_entry))->pde_unload_lock)->rlock)->magic: 0xdead4ead
	// ((&(kmem_cache#29-oX (struct proc_dir_entry))->pde_unload_lock)->rlock)->owner: 0xffffffff
	// ((&(kmem_cache#29-oX (struct proc_dir_entry))->pde_unload_lock)->rlock)->owner_cpu: 0xffffffff
	// &(kmem_cache#29-oX (struct proc_dir_entry))->pde_openers->i_sb_list->next: &(kmem_cache#29-oX (struct proc_dir_entry))->pde_openers->i_sb_list
	// &(kmem_cache#29-oX (struct proc_dir_entry))->pde_openers->i_sb_list->prev: &(kmem_cache#29-oX (struct proc_dir_entry))->pde_openers->i_sb_list
	//
	// parent: &proc_root

	// proc_sys_root->proc_iops: (kmem_cache#29-oX (struct proc_dir_entry))->proc_iops
	proc_sys_root->proc_iops = &proc_sys_dir_operations;
	// proc_sys_root->proc_iops: (kmem_cache#29-oX (struct proc_dir_entry))->proc_iops: &proc_sys_dir_operations

	// proc_sys_root->proc_fops: (kmem_cache#29-oX (struct proc_dir_entry))->proc_fops
	proc_sys_root->proc_fops = &proc_sys_dir_file_operations;
	// proc_sys_root->proc_fops: (kmem_cache#29-oX (struct proc_dir_entry))->proc_fops: &proc_sys_dir_file_operations

	// proc_sys_root->nlink: (kmem_cache#29-oX (struct proc_dir_entry))->nlink: 2
	proc_sys_root->nlink = 0;
	// proc_sys_root->nlink: (kmem_cache#29-oX (struct proc_dir_entry))->nlink: 0

	// sysctl_init(): 0
	return sysctl_init();
	// return 0

	// sysctl_init 에서 한일:
	// struct ctl_table_header 의 메모리 +  struct ctl_table_header 의 포인터 메모리 * 2 를 할당 받음 kmem_cache#27-oX
	//
	// struct ctl_table_header 의 메모리 + struct ctl_node 의 메모리 46 개를 할당 받음 kmem_cache#25-oX
	//
	// (kmem_cache#25-oX)->ctl_table: kmem_cache#24-oX
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#25-oX)->used: 0
	// (kmem_cache#25-oX)->count: 1
	// (kmem_cache#25-oX)->nreg: 1
	// (kmem_cache#25-oX)->unregistering: NULL
	// (kmem_cache#25-oX)->root: &sysctl_table_root
	// (kmem_cache#25-oX)->set: &sysctl_table_root.default_set
	// (kmem_cache#25-oX)->parent: NULL
	// (kmem_cache#25-oX)->node: &(kmem_cache#25-oX)[1] (struct ctl_node)
	// (&(kmem_cache#25-oX)[1...46] (struct ctl_node))->header: kmem_cache#25-oX
	//
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 2
	//
	// struct ctl_dir: 36, struct ctl_node: 16, struct ctl_table: 34 * 2, char: 7
	// 만큼의 메모리 kmem_cache#29-oX 를 할당 받음
	//
	// (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).procname: (kmem_cache#29-oX + 120): "kernel"
	// ((kmem_cache#29-oX + 52)[0] (struct ctl_table)).mode: 0040555
	// (&(kmem_cache#29-oX)->header)->ctl_table: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->ctl_table_arg: (kmem_cache#29-oX + 52) (struct ctl_table)
	// (&(kmem_cache#29-oX)->header)->used: 0
	// (&(kmem_cache#29-oX)->header)->count: 1
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	// (&(kmem_cache#29-oX)->header)->unregistering: NULL
	// (&(kmem_cache#29-oX)->header)->root: (&sysctl_table_root.default_set)->dir.header.root
	// (&(kmem_cache#29-oX)->header)->set: &sysctl_table_root.default_set
	// (&(kmem_cache#29-oX)->header)->parent: NULL
	// (&(kmem_cache#29-oX)->header)->node: (kmem_cache#29-oX + 36) (struct ctl_node)
	// ((kmem_cache#29-oX + 36) (struct ctl_node))->header: &(kmem_cache#29-oX)->header
	//
	// (&(&sysctl_table_root.default_set)->dir)->header.nreg: 3
	// (&(kmem_cache#29-oX)->header)->parent: &(&sysctl_table_root.default_set)->dir
	//
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node).__rb_parent_color: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_left: NULL
	// (&((kmem_cache#29-oX + 36) (struct ctl_node)).node)->rb_right: NULL
	// (&(&sysctl_table_root.default_set)->dir)->root.rb_node: &((kmem_cache#29-oX + 36) (struct ctl_node)).node
	//
	// RB Tree &((kmem_cache#29-oX + 36) (struct ctl_node)).node 을 black node 로 추가
	/*
	//                          proc-b
	//                         (kernel)
	*/
	// (&(&(&sysctl_table_root.default_set)->dir)->header)->nreg: 2
	// (&(kmem_cache#29-oX)->header)->nreg: 1
	//
	// (kmem_cache#29-oX)->header.nreg: 2
	// (kmem_cache#25-oX)->parent: kmem_cache#29-oX
	//
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node).__rb_parent_color: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: NULL
	// &(kmem_cache#29-oX)->root.rb_node: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node).__rb_parent_color: &(&(kmem_cache#25-oX)[1] (struct ctl_node)).node
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_left: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node)->rb_right: NULL
	// (&(&(kmem_cache#25-oX)[1] (struct ctl_node)).node)->rb_right: &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node
	//
	// &(&(kmem_cache#25-oX)[1] (struct ctl_node) + 1).node 을 node 로 추가 후 rbtree 로 구성
	// (kern_table 의 2 번째 index의 값)
	/*
	//                       kern_table-b
	//                 (sched_child_runs_first)
	//                                      \
	//                                        kern_table-r
	//                                  (sched_min_granularity_ns)
	//
	// ..... kern_table 의 index 수만큼 RB Tree를 구성
	// 아래 링크의 RB Tree 그림 참고
	// http://neuromancer.kr/t/150-2016-07-09-proc-root-init/313
	//
	// TODO: kern_table 의 rbtree 그림을 그려야함
	*/
	// (kmem_cache#25-oX)->ctl_table_arg: kmem_cache#24-oX
	// (kmem_cache#27-oX)[1] (struct ctl_table_header): kmem_cache#25-oX
	//
	// kern_table index 만큼 kern_table[...].child 맴버값을 보고 dir, files 의 RB Tree 를 구성함
	//
	// sysctl_base_table index 만큼 sysctl_base_table[...].child 맴버값을 보고 recursive 하게 dir, files 의 RB Tree 를 구성함
}
