#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

#define HYDRA_AD_MASK (_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SAVED_DIRTY)
#define HYDRA_CHAIN_CAP (2 * NUMA_NODE_COUNT)
#define HYDRA_AUDIT_DETAIL_CAP 16

static DEFINE_MUTEX(hydra_verify_lock);
static pid_t hydra_walk_pid;
static unsigned long hydra_walk_addr;
static pid_t hydra_audit_pid;

struct hydra_audit_ctr {
	long pud_regions;
	long pte_leaves;
	long pmd_leaves;
	long members;
	long v_ring;
	long v_dupnid;
	long v_parked_ref;
	long v_foreign_ref;
	long v_map;
	long v_diverge;
	long v_nonzero_nonpresent;
	long v_child_not_in_chain;
	long a_master_nid;
	long a_entry_no_master;
	int details;
};

static struct mm_struct *hydra_verify_mm(pid_t pid, struct task_struct **taskp)
{
	struct task_struct *task;
	struct mm_struct *mm;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (!task)
		return NULL;

	mm = get_task_mm(task);
	if (!mm) {
		put_task_struct(task);
		return NULL;
	}

	*taskp = task;
	return mm;
}

static int hydra_chain_members(struct page *start, struct page **out)
{
	struct page *cur = start;
	int n = 0;

	do {
		if (n >= HYDRA_CHAIN_CAP)
			return -1;
		out[n++] = cur;
		cur = READ_ONCE(cur->next_replica);
	} while (cur && cur != start);

	if (cur != start && n > 1)
		return -1;

	return n;
}

static bool hydra_page_in_chain(struct page *start, struct page *needle)
{
	struct page *cur = start;
	int n = 0;

	do {
		if (cur == needle)
			return true;
		if (++n >= HYDRA_CHAIN_CAP)
			return false;
		cur = READ_ONCE(cur->next_replica);
	} while (cur && cur != start);

	return false;
}

static void hydra_print_chain(struct seq_file *m, struct page *start)
{
	struct page *members[HYDRA_CHAIN_CAP];
	int n, i;

	rcu_read_lock();
	n = hydra_chain_members(start, members);
	if (n < 0) {
		rcu_read_unlock();
		seq_puts(m, "  chain: NOT A CLOSED RING\n");
		return;
	}

	seq_printf(m, "\n  leaf chain (%d members)  [first row = entry walked from]\n", n);
	seq_puts(m, "    page               nid  parked  rent      map_nodes\n");
	for (i = 0; i < n; i++) {
		struct page *p = members[i];

		seq_printf(m, "    %-18px %-4d %-7d %-9d 0x%02lx\n",
			   p, page_to_nid(p), hydra_pt_parked(p) ? 1 : 0,
			   atomic_read(&p->hydra_rent),
			   READ_ONCE(p->hydra_map_nodes) & HYDRA_MAP_NODES_MASK);
	}
	rcu_read_unlock();
}

static int hydra_walk_show(struct seq_file *m, void *v)
{
	struct task_struct *task = NULL;
	struct mm_struct *mm;
	struct page *master_leaf = NULL;
	struct page *master_pmd_page = NULL;
	unsigned long addr;
	pid_t pid;
	void *owner_e;
	int owner = -1;
	int j;

	mutex_lock(&hydra_verify_lock);
	pid = hydra_walk_pid;
	addr = hydra_walk_addr;
	mutex_unlock(&hydra_verify_lock);

	if (pid <= 0) {
		seq_puts(m, " no target: write \"<pid> <hex-vaddr>\" first\n");
		return 0;
	}

	mm = hydra_verify_mm(pid, &task);
	if (!mm) {
		seq_printf(m, " pid %d: no such task or no mm\n", pid);
		return 0;
	}

	if (mmap_read_lock_killable(mm)) {
		mmput(mm);
		put_task_struct(task);
		return -EINTR;
	}

	seq_puts(m, " Hydra VA walk\n");
	seq_printf(m, "  %-18s %d\n", "pid", pid);
	seq_printf(m, "  %-18s %s\n", "comm", task->comm);
	seq_printf(m, "  %-18s 0x%016lx\n", "addr", addr);
	seq_printf(m, "  %-18s %px\n", "mm", mm);
	seq_printf(m, "  %-18s %d\n", "repl enabled",
		   mm->lazy_repl_enabled ? 1 : 0);

	if (mm->lazy_repl_enabled && mm->hydra_pud_owner) {
		owner_e = xa_load(mm->hydra_pud_owner, addr >> PUD_SHIFT);
		if (owner_e)
			owner = (int)xa_to_value(owner_e);
	}
	seq_printf(m, "  %-18s %d\n", "pud owner node", owner);

	seq_puts(m, "  steering          ");
	for (j = 0; j < NUMA_NODE_COUNT; j++)
		seq_printf(m, " n%d->%d", j, READ_ONCE(mm->repl_steering[j]));
	seq_putc(m, '\n');

	seq_puts(m, "\n  per-node tree walk  [rows = replica tree; cols = nid of table page per level]\n");
	seq_puts(m, "    tree p4d  pud  pmd  leaf state         entry_raw          pfn\n");

	for (j = 0; j < NUMA_NODE_COUNT; j++) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pmd_t pmdval;

		if (!mm->repl_pgd[j]) {
			seq_printf(m, "    n%-3d no tree\n", j);
			continue;
		}

		pgd = pgd_offset_pgd(mm->repl_pgd[j], addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd)) {
			seq_printf(m, "    n%-3d -    -    -    -    NOPATH(pgd)\n", j);
			continue;
		}
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			seq_printf(m, "    n%-3d n%-3d -    -    -    NOPATH(p4d)\n",
				   j, page_to_nid(virt_to_page(p4d)));
			continue;
		}
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_trans_huge(*pud) || pud_bad(*pud)) {
			seq_printf(m, "    n%-3d n%-3d n%-3d -    -    NOPATH(pud)\n",
				   j, page_to_nid(virt_to_page(p4d)),
				   page_to_nid(virt_to_page(pud)));
			continue;
		}
		pmd = pmd_offset(pud, addr);
		pmdval = READ_ONCE(*pmd);

		if (pmd_none(pmdval)) {
			seq_printf(m, "    n%-3d n%-3d n%-3d n%-3d -    NONE\n",
				   j, page_to_nid(virt_to_page(p4d)),
				   page_to_nid(virt_to_page(pud)),
				   page_to_nid(virt_to_page(pmd)));
			if (j == owner || (owner < 0 && j == 0))
				master_pmd_page = virt_to_page(pmd);
			continue;
		}

		if (pmd_trans_huge(pmdval)) {
			seq_printf(m, "    n%-3d n%-3d n%-3d n%-3d huge HUGE(2M)      0x%016lx 0x%lx\n",
				   j, page_to_nid(virt_to_page(p4d)),
				   page_to_nid(virt_to_page(pud)),
				   page_to_nid(virt_to_page(pmd)),
				   pmd_val(pmdval),
				   (unsigned long)pmd_pfn(pmdval));
			if (j == owner || (owner < 0 && j == 0))
				master_pmd_page = virt_to_page(pmd);
			continue;
		}

		if (pmd_bad(pmdval)) {
			seq_printf(m, "    n%-3d BAD pmd 0x%016lx\n", j,
				   pmd_val(pmdval));
			continue;
		}

		{
			struct page *leaf = pmd_pgtable(pmdval);
			pte_t *pte = pte_offset_kernel(pmd, addr);
			pte_t pteval = READ_ONCE(*pte);
			int lnid = page_to_nid(leaf);
			char state[24];

			if (j == owner) {
				scnprintf(state, sizeof(state), "MASTER");
				master_leaf = leaf;
			} else if (lnid == j) {
				scnprintf(state, sizeof(state), "SYNC");
			} else {
				scnprintf(state, sizeof(state), "SHARED->n%d", lnid);
			}
			if (!master_leaf && owner < 0)
				master_leaf = leaf;
			if (hydra_pt_parked(leaf))
				strlcat(state, "(PARKED!)", sizeof(state));

			seq_printf(m, "    n%-3d n%-3d n%-3d n%-3d n%-3d %-13s 0x%016lx 0x%lx\n",
				   j, page_to_nid(virt_to_page(p4d)),
				   page_to_nid(virt_to_page(pud)),
				   page_to_nid(virt_to_page(pmd)), lnid, state,
				   pte_val(pteval),
				   pte_present(pteval) ?
				   (unsigned long)pte_pfn(pteval) : 0);
		}
	}

	if (master_leaf)
		hydra_print_chain(m, master_leaf);
	else if (master_pmd_page)
		hydra_print_chain(m, master_pmd_page);

	mmap_read_unlock(mm);
	mmput(mm);
	put_task_struct(task);
	return 0;
}

static void hydra_audit_detail(struct seq_file *m, struct hydra_audit_ctr *c,
			       const char *what, unsigned long addr, int node)
{
	if (c->details >= HYDRA_AUDIT_DETAIL_CAP)
		return;
	c->details++;
	seq_printf(m, "  DETAIL %-26s addr 0x%016lx node %d\n", what, addr, node);
}

static void hydra_audit_refs(struct seq_file *m, struct mm_struct *mm,
			     unsigned long addr, struct page *master,
			     struct page **members, int nmem,
			     struct hydra_audit_ctr *c)
{
	unsigned long refs = 0, covered = 0;
	int k, i;

	for (k = 0; k < NUMA_NODE_COUNT; k++) {
		pmd_t *pmd_k = hydra_walk_to_pmd(mm, addr, k);
		struct page *p;

		if (HYDRA_WALK_BAD(pmd_k))
			continue;
		if (pmd_none(*pmd_k) || pmd_trans_huge(*pmd_k) ||
		    pmd_bad(*pmd_k))
			continue;
		p = pmd_pgtable(*pmd_k);
		refs |= 1UL << k;

		if (hydra_pt_parked(p)) {
			c->v_parked_ref++;
			hydra_audit_detail(m, c, "parked-but-referenced", addr, k);
		}
		if (!hydra_page_in_chain(master, p)) {
			c->v_foreign_ref++;
			hydra_audit_detail(m, c, "ref-outside-chain", addr, k);
		}
	}

	covered = READ_ONCE(master->hydra_map_nodes) & HYDRA_MAP_NODES_MASK;
	for (i = 0; i < nmem; i++)
		covered |= 1UL << page_to_nid(members[i]);

	if (refs & ~covered) {
		c->v_map++;
		hydra_audit_detail(m, c, "flush-map-not-superset", addr,
				   __builtin_ffsl(refs & ~covered) - 1);
	}
}

static void hydra_audit_pte_leaf(struct seq_file *m, struct mm_struct *mm,
				 unsigned long addr, pmd_t *master_pmd,
				 struct hydra_audit_ctr *c)
{
	struct page *members[HYDRA_CHAIN_CAP];
	struct page *master;
	spinlock_t *pml, *ptl;
	unsigned long seen_nids = 0;
	pte_t *mbase;
	int nmem, i, e;

	pml = pmd_lock(mm, master_pmd);
	if (pmd_none(*master_pmd) || pmd_trans_huge(*master_pmd) ||
	    pmd_bad(*master_pmd)) {
		spin_unlock(pml);
		return;
	}
	ptl = pte_lockptr(mm, master_pmd);
	if (ptl != pml)
		spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

	master = pmd_pgtable(*master_pmd);
	c->pte_leaves++;

	rcu_read_lock();
	nmem = hydra_chain_members(master, members);
	rcu_read_unlock();
	if (nmem < 0) {
		c->v_ring++;
		hydra_audit_detail(m, c, "chain-not-a-ring", addr,
				   page_to_nid(master));
		goto out;
	}
	c->members += nmem;

	for (i = 0; i < nmem; i++) {
		int nid = page_to_nid(members[i]);

		if (seen_nids & (1UL << nid)) {
			c->v_dupnid++;
			hydra_audit_detail(m, c, "duplicate-nid-in-chain", addr, nid);
		}
		seen_nids |= 1UL << nid;
	}

	hydra_audit_refs(m, mm, addr, master, members, nmem, c);

	mbase = (pte_t *)page_address(master);
	for (e = 0; e < PTRS_PER_PTE; e++) {
		pte_t mval = READ_ONCE(mbase[e]);

		for (i = 0; i < nmem; i++) {
			pte_t cval;

			if (members[i] == master || hydra_pt_parked(members[i]))
				continue;
			cval = READ_ONCE(((pte_t *)page_address(members[i]))[e]);

			if (!(pte_val(mval) & _PAGE_PRESENT)) {
				if (pte_val(cval)) {
					c->v_nonzero_nonpresent++;
					hydra_audit_detail(m, c,
						"replica-set-master-clear",
						addr + ((unsigned long)e << PAGE_SHIFT),
						page_to_nid(members[i]));
				}
				continue;
			}
			if (!pte_val(cval))
				continue;
			if (!(pte_val(cval) & _PAGE_PRESENT) ||
			    ((pte_val(cval) ^ pte_val(mval)) & ~HYDRA_AD_MASK)) {
				c->v_diverge++;
				hydra_audit_detail(m, c, "replica-divergence",
					addr + ((unsigned long)e << PAGE_SHIFT),
					page_to_nid(members[i]));
			}
		}
	}

out:
	if (ptl != pml)
		spin_unlock(ptl);
	spin_unlock(pml);
}

static void hydra_audit_pmd_table(struct seq_file *m, struct mm_struct *mm,
				  unsigned long addr, pud_t *master_pud,
				  struct hydra_audit_ctr *c)
{
	struct page *members[HYDRA_CHAIN_CAP];
	struct page *master;
	pmd_t *mbase;
	spinlock_t *pml;
	unsigned long seen_nids = 0;
	int nmem, i, e;

	if (pud_none(*master_pud) || pud_trans_huge(*master_pud) ||
	    pud_bad(*master_pud))
		return;

	mbase = pmd_offset(master_pud, addr & PUD_MASK);
	master = virt_to_page(mbase);
	pml = pmd_lockptr(mm, mbase);
	spin_lock(pml);
	c->pmd_leaves++;

	rcu_read_lock();
	nmem = hydra_chain_members(master, members);
	rcu_read_unlock();
	if (nmem < 0) {
		c->v_ring++;
		hydra_audit_detail(m, c, "pmd-chain-not-a-ring", addr,
				   page_to_nid(master));
		goto out;
	}
	c->members += nmem;

	for (i = 0; i < nmem; i++) {
		int nid = page_to_nid(members[i]);

		if (seen_nids & (1UL << nid)) {
			c->v_dupnid++;
			hydra_audit_detail(m, c, "duplicate-nid-in-pmd-chain",
					   addr, nid);
		}
		seen_nids |= 1UL << nid;
	}

	for (e = 0; e < PTRS_PER_PMD; e++) {
		pmd_t mval = READ_ONCE(mbase[e]);
		unsigned long eaddr = (addr & PUD_MASK) +
				      ((unsigned long)e << PMD_SHIFT);
		bool mhuge = (pmd_val(mval) & _PAGE_PRESENT) &&
			     (pmd_trans_huge(mval) || pmd_leaf(mval));

		for (i = 0; i < nmem; i++) {
			pmd_t cval;

			if (members[i] == master || hydra_pt_parked(members[i]))
				continue;
			cval = READ_ONCE(((pmd_t *)page_address(members[i]))[e]);
			if (!pmd_val(cval))
				continue;

			if (pmd_trans_huge(cval) || pmd_leaf(cval)) {
				if (!mhuge ||
				    ((pmd_val(cval) ^ pmd_val(mval)) & ~HYDRA_AD_MASK)) {
					c->v_diverge++;
					hydra_audit_detail(m, c,
						"huge-pmd-divergence", eaddr,
						page_to_nid(members[i]));
				}
				continue;
			}

			{
				struct page *child = pmd_pgtable(cval);
				struct page *mchild;
				pmd_t me = READ_ONCE(mbase[e]);

				if (!(pmd_val(me) & _PAGE_PRESENT) ||
				    pmd_trans_huge(me) || pmd_leaf(me)) {
					c->a_entry_no_master++;
					continue;
				}
				mchild = pmd_pgtable(me);
				if (!hydra_page_in_chain(mchild, child)) {
					c->v_child_not_in_chain++;
					hydra_audit_detail(m, c,
						"child-not-in-master-chain",
						eaddr, page_to_nid(members[i]));
				}
			}
		}
	}

out:
	spin_unlock(pml);
}

static int hydra_audit_show(struct seq_file *m, void *v)
{
	struct task_struct *task = NULL;
	struct mm_struct *mm;
	struct hydra_audit_ctr c;
	unsigned long addr, end, next_pud;
	pid_t pid;
	long viol, anom;

	mutex_lock(&hydra_verify_lock);
	pid = hydra_audit_pid;
	mutex_unlock(&hydra_verify_lock);

	if (pid <= 0) {
		seq_puts(m, " no target: write \"<pid>\" first\n");
		return 0;
	}

	mm = hydra_verify_mm(pid, &task);
	if (!mm) {
		seq_printf(m, " pid %d: no such task or no mm\n", pid);
		return 0;
	}

	memset(&c, 0, sizeof(c));

	if (mmap_read_lock_killable(mm)) {
		mmput(mm);
		put_task_struct(task);
		return -EINTR;
	}

	seq_puts(m, " Hydra coherence audit\n");
	seq_printf(m, "  %-18s %d\n", "pid", pid);
	seq_printf(m, "  %-18s %s\n", "comm", task->comm);
	seq_printf(m, "  %-18s %d\n", "repl enabled",
		   mm->lazy_repl_enabled ? 1 : 0);

	if (!mm->lazy_repl_enabled)
		goto summary;

	addr = 0;
	end = mm->task_size;
	while (addr < end) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		void *owner_e;

		next_pud = (addr & PUD_MASK) + PUD_SIZE;
		if (!next_pud || next_pud > end)
			next_pud = end;

		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd)) {
			addr = next_pud;
			continue;
		}
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			addr = next_pud;
			continue;
		}
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_trans_huge(*pud) || pud_bad(*pud)) {
			addr = next_pud;
			continue;
		}
		c.pud_regions++;

		if (mm->hydra_pud_owner) {
			owner_e = xa_load(mm->hydra_pud_owner, addr >> PUD_SHIFT);
			if (owner_e &&
			    (int)xa_to_value(owner_e) !=
			    page_to_nid(virt_to_page(pmd_offset(pud, addr))))
				c.a_master_nid++;
		}

		hydra_audit_pmd_table(m, mm, addr, pud, &c);

		pmd = pmd_offset(pud, addr);
		while (addr < next_pud) {
			pmd_t pv = READ_ONCE(*pmd);

			if (!pmd_none(pv) && !pmd_trans_huge(pv) &&
			    !pmd_bad(pv))
				hydra_audit_pte_leaf(m, mm, addr & PMD_MASK,
						     pmd, &c);
			addr = (addr & PMD_MASK) + PMD_SIZE;
			pmd++;
		}
	}

summary:
	viol = c.v_ring + c.v_dupnid + c.v_parked_ref + c.v_foreign_ref +
	       c.v_map + c.v_diverge + c.v_nonzero_nonpresent +
	       c.v_child_not_in_chain;
	anom = c.a_master_nid + c.a_entry_no_master;

	seq_puts(m, "\n  scanned\n");
	seq_printf(m, "    %-32s %8ld\n", "pud regions", c.pud_regions);
	seq_printf(m, "    %-32s %8ld\n", "pmd tables", c.pmd_leaves);
	seq_printf(m, "    %-32s %8ld\n", "pte leaves", c.pte_leaves);
	seq_printf(m, "    %-32s %8ld\n", "chain members", c.members);
	seq_puts(m, "\n  invariant violations\n");
	seq_printf(m, "    %-32s %8ld\n", "chain not a ring", c.v_ring);
	seq_printf(m, "    %-32s %8ld\n", "duplicate nid in chain", c.v_dupnid);
	seq_printf(m, "    %-32s %8ld\n", "parked but referenced", c.v_parked_ref);
	seq_printf(m, "    %-32s %8ld\n", "reference outside chain", c.v_foreign_ref);
	seq_printf(m, "    %-32s %8ld\n", "flush map not superset", c.v_map);
	seq_printf(m, "    %-32s %8ld\n", "replica value divergence", c.v_diverge);
	seq_printf(m, "    %-32s %8ld\n", "replica set, master clear",
		   c.v_nonzero_nonpresent);
	seq_printf(m, "    %-32s %8ld\n", "child not in master chain",
		   c.v_child_not_in_chain);
	seq_puts(m, "\n  anomalies (informational)\n");
	seq_printf(m, "    %-32s %8ld\n", "master pmd nid != pud owner",
		   c.a_master_nid);
	seq_printf(m, "    %-32s %8ld\n", "replica entry, master no table",
		   c.a_entry_no_master);
	seq_printf(m, "\n  verdict: %s (%ld violations, %ld anomalies)\n",
		   viol ? "FAIL" : "PASS", viol, anom);

	mmap_read_unlock(mm);
	mmput(mm);
	put_task_struct(task);
	return 0;
}

static ssize_t hydra_walk_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char buf[64];
	size_t len;
	int pid;
	unsigned long addr;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%d %lx", &pid, &addr) != 2 || pid <= 0)
		return -EINVAL;

	mutex_lock(&hydra_verify_lock);
	hydra_walk_pid = pid;
	hydra_walk_addr = addr;
	mutex_unlock(&hydra_verify_lock);

	return count;
}

static ssize_t hydra_audit_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	char buf[32];
	size_t len;
	int pid;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sscanf(buf, "%d", &pid) != 1 || pid <= 0)
		return -EINVAL;

	mutex_lock(&hydra_verify_lock);
	hydra_audit_pid = pid;
	mutex_unlock(&hydra_verify_lock);

	return count;
}

static int hydra_walk_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, hydra_walk_show, NULL, 64 * 1024);
}

static int hydra_audit_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, hydra_audit_show, NULL, 64 * 1024);
}

const struct proc_ops hydra_walk_proc_ops = {
	.proc_open	= hydra_walk_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_walk_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

const struct proc_ops hydra_audit_proc_ops = {
	.proc_open	= hydra_audit_open,
	.proc_read	= seq_read,
	.proc_write	= hydra_audit_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};
