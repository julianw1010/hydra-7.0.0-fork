#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hydra.h>

extern int sysctl_hydra_repl_order;
extern int sysctl_hydra_tlbflush_opt;

static struct proc_dir_entry *hydra_dir;

static int hydra_cache_show(struct seq_file *m, void *v)
{
	int node;

	seq_printf(m, "%-6s %8s %12s %12s %12s\n",
		   "node", "count", "hits", "misses", "returns");

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct hydra_cache_head *c = &hydra_cache[node];

		seq_printf(m, "%-6d %8d %12lld %12lld %12lld\n",
			   node,
			   atomic_read(&c->count),
			   atomic64_read(&c->hits),
			   atomic64_read(&c->misses),
			   atomic64_read(&c->returns));
	}

	return 0;
}

static int hydra_cache_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_cache_show, NULL);
}

static ssize_t hydra_cache_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;
	int node, added, total, drained;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val == -1) {
		drained = hydra_cache_drain_all();
		pr_info("HYDRA: cache drained %d pages\n", drained);
		return count;
	}

	if (val <= 0 || val > 131072)
		return -EINVAL;

	total = 0;
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		if (!node_online(node))
			continue;

		added = 0;
		while (added < val) {
			struct page *page;

			page = alloc_pages_node(node,
				GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE, 0);
			if (!page)
				break;

			if (page_to_nid(page) != node) {
				__free_page(page);
				break;
			}

			page->next_replica = NULL;
			page->pt_owner_mm = NULL;

			if (!hydra_cache_push(page, node)) {
				__free_page(page);
				break;
			}

			added++;
		}

		total += added;
	}

	pr_info("HYDRA: cache populated %d pages across %d nodes\n",
		total, num_online_nodes());

	return count;
}

static const struct proc_ops hydra_cache_ops = {
	.proc_open	= hydra_cache_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_cache_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_repl_order_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_hydra_repl_order);
	return 0;
}

static int hydra_repl_order_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_repl_order_show, NULL);
}

static ssize_t hydra_repl_order_write(struct file *file, const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 9)
		return -EINVAL;

	sysctl_hydra_repl_order = (int)val;

	pr_info("HYDRA: repl_order set to %d\n", sysctl_hydra_repl_order);

	return count;
}

static const struct proc_ops hydra_repl_order_ops = {
	.proc_open	= hydra_repl_order_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_repl_order_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_tlbflush_opt_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_hydra_tlbflush_opt);
	return 0;
}

static int hydra_tlbflush_opt_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_tlbflush_opt_show, NULL);
}

static ssize_t hydra_tlbflush_opt_write(struct file *file, const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	sysctl_hydra_tlbflush_opt = (int)val;

	pr_info("HYDRA: tlbflush_opt set to %d\n", sysctl_hydra_tlbflush_opt);

	return count;
}

static const struct proc_ops hydra_tlbflush_opt_ops = {
	.proc_open	= hydra_tlbflush_opt_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_tlbflush_opt_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_verify_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", sysctl_hydra_verify);
	return 0;
}

static int hydra_verify_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_verify_show, NULL);
}

static ssize_t hydra_verify_write(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long val;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > 1)
		return -EINVAL;

	WRITE_ONCE(sysctl_hydra_verify, (int)val);

	pr_info("HYDRA: verify set to %d\n", sysctl_hydra_verify);

	return count;
}

static const struct proc_ops hydra_verify_ops = {
	.proc_open	= hydra_verify_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_verify_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static const struct proc_ops hydra_status_ops = {
	.proc_open	= hydra_status_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static const struct proc_ops hydra_history_ops = {
	.proc_open	= hydra_history_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};

static int hydra_balance_show(struct seq_file *m, void *v)
{
	static const char * const lvl[HYDRA_PT_NR_LEVELS] = {
		[HYDRA_PT_PGD] = "PGD",
		[HYDRA_PT_P4D] = "P4D",
		[HYDRA_PT_PUD] = "PUD",
		[HYDRA_PT_PMD] = "PMD",
		[HYDRA_PT_PTE] = "PTE",
	};
	long total_alloc = 0, total_free = 0;
	int level;

	seq_puts(m, " Hydra page-table page balance  (every tracked page-table page)\n");
	seq_puts(m, " Hydra has no stable replica-vs-master page identity (the role is\n");
	seq_puts(m, " per-VMA and can be reassigned), so this counts ALL page-table pages,\n");
	seq_puts(m, " not just replicas.\n");
	seq_puts(m, " rows = page-table level,  cols = cumulative count since boot\n");
	seq_puts(m, " LIVE = ALLOCS - FREES;  must never go negative, and must return to\n");
	seq_puts(m, " its steady-state baseline once a workload exits (else pages leaked).\n");
	seq_puts(m, " -----------------------------------------------------------------------\n");
	seq_printf(m, " %-6s %15s %15s %15s\n", "level", "allocs", "frees", "live");

	for (level = HYDRA_PT_PGD; level < HYDRA_PT_NR_LEVELS; level++) {
		long a = atomic_long_read(&hydra_pt_allocs[level]);
		long f = atomic_long_read(&hydra_pt_frees[level]);

		total_alloc += a;
		total_free += f;
		seq_printf(m, " %-6s %15ld %15ld %15ld\n", lvl[level], a, f, a - f);
	}

	seq_puts(m, " -----------------------------------------------------------------------\n");
	seq_printf(m, " %-6s %15ld %15ld %15ld\n", "TOTAL",
		   total_alloc, total_free, total_alloc - total_free);
	return 0;
}

static int hydra_balance_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_balance_show, NULL);
}

static const struct proc_ops hydra_balance_ops = {
	.proc_open	= hydra_balance_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int hydra_audit_show(struct seq_file *m, void *v)
{
	hydra_audit_seq_show(m);
	return 0;
}

static int hydra_audit_open(struct inode *inode, struct file *file)
{
	return single_open(file, hydra_audit_show, NULL);
}

static ssize_t hydra_audit_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	long pid;
	int ret;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (kstrtol(buf, 10, &pid))
		return -EINVAL;
	if (pid < 0)
		return -EINVAL;

	ret = hydra_audit_run((pid_t)pid);
	if (ret)
		return ret;

	return count;
}

static const struct proc_ops hydra_audit_ops = {
	.proc_open	= hydra_audit_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_audit_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init hydra_proc_init(void)
{
	hydra_dir = proc_mkdir("hydra", NULL);
	if (!hydra_dir)
		return -ENOMEM;

	if (!proc_create("cache", 0644, hydra_dir, &hydra_cache_ops))
		goto fail;

	if (!proc_create("repl_order", 0644, hydra_dir, &hydra_repl_order_ops))
		goto fail;

	if (!proc_create("tlbflush_opt", 0644, hydra_dir, &hydra_tlbflush_opt_ops))
		goto fail;

	if (!proc_create("verify", 0644, hydra_dir, &hydra_verify_ops))
		goto fail;

	if (!proc_create("status", 0444, hydra_dir, &hydra_status_ops))
		goto fail;

	if (!proc_create("history", 0444, hydra_dir, &hydra_history_ops))
		goto fail;

	if (!proc_create("balance", 0444, hydra_dir, &hydra_balance_ops))
		goto fail;

	if (!proc_create("audit", 0644, hydra_dir, &hydra_audit_ops))
		goto fail;

	return 0;

fail:
	remove_proc_subtree("hydra", NULL);
	return -ENOMEM;
}
late_initcall(hydra_proc_init);
