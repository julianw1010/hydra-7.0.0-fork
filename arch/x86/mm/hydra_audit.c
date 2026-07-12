#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/nodemask.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

#define HYDRA_AUDIT_AD (_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SAVED_DIRTY)

struct hydra_audit_result {
	int valid;
	int pid;
	int enabled;
	int nodes;
	long pmd_pages;
	long pmd_members;
	long pte_pages;
	long pte_members;
	long leaf_entries;
	long table_entries;
};

static DEFINE_SPINLOCK(hydra_audit_lock);
static struct hydra_audit_result hydra_audit_last;

struct audit_ctx {
	struct mm_struct *mm;
	long pmd_pages;
	long pmd_members;
	long pte_pages;
	long pte_members;
	long leaf_entries;
	long table_entries;
};

static void audit_fail(struct mm_struct *mm, unsigned long addr, const char *what,
		       int node, unsigned long expected, unsigned long got)
{
	pr_emerg("HYDRA audit: mm %px addr %lx %s node %d expected %lx got %lx\n",
		 mm, addr, what, node, expected, got);
	BUG();
}

static struct page *audit_member_on_node(struct page *base, int node)
{
	struct page *cur = base;
	struct page *start = base;

	do {
		if (page_to_nid(cur) == node)
			return cur;
		cur = READ_ONCE(cur->next_replica);
	} while (cur && cur != start);

	return NULL;
}

static long audit_ring(struct audit_ctx *c, struct page *p, int level,
		       unsigned long addr)
{
	struct page *cur, *start;
	nodemask_t seen;
	long count = 0;

	if (!READ_ONCE(p->next_replica))
		return 1;

	nodes_clear(seen);
	start = p;
	cur = p;
	do {
		int n = page_to_nid(cur);

		if (n < 0 || n >= NUMA_NODE_COUNT)
			audit_fail(c->mm, addr, "ring member bad node", n, 0, 0);
		if (node_isset(n, seen))
			audit_fail(c->mm, addr, "ring duplicate node", n, 0, 0);
		if (cur->pt_owner_mm != c->mm)
			audit_fail(c->mm, addr, "ring member wrong owner_mm", n,
				   (unsigned long)c->mm,
				   (unsigned long)cur->pt_owner_mm);
		node_set(n, seen);
		if (++count > NUMA_NODE_COUNT)
			audit_fail(c->mm, addr, "ring overlong", level,
				   NUMA_NODE_COUNT, count);
		cur = READ_ONCE(cur->next_replica);
	} while (cur && cur != start);

	if (cur != start)
		audit_fail(c->mm, addr, "ring not closed", level, 0, 0);

	return count;
}

static void audit_pte_page(struct audit_ctx *c, struct page *master_pte,
			   unsigned long base, int tree_node, pmd_t *parent_pmd)
{
	unsigned long *mbase;
	spinlock_t *ptl;
	long members;
	int i;

	if (page_to_nid(master_pte) != tree_node)
		audit_fail(c->mm, base, "PTE table off node", tree_node,
			   tree_node, page_to_nid(master_pte));

	members = audit_ring(c, master_pte, HYDRA_PT_PTE, base);
	c->pte_pages++;
	c->pte_members += members;

	mbase = page_address(master_pte);
	ptl = pte_lockptr(c->mm, parent_pmd);

	spin_lock(ptl);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		unsigned long mval = READ_ONCE(mbase[i]);
		unsigned long off = (unsigned long)i * sizeof(pte_t);
		unsigned long a = base + ((unsigned long)i << PAGE_SHIFT);
		bool present = mval != 0 && pte_present(__pte(mval));
		bool expect_zero = mval == 0 || !present;
		struct page *cur, *start;

		start = master_pte;
		cur = READ_ONCE(master_pte->next_replica);
		while (cur && cur != start) {
			unsigned long v =
				READ_ONCE(*(unsigned long *)(page_address(cur) + off));
			int n = page_to_nid(cur);

			if (expect_zero) {
				if (v != 0)
					audit_fail(c->mm, a,
						   "PTE replica present, master absent",
						   n, 0, v);
			} else if (v != 0 &&
				   (v & ~HYDRA_AUDIT_AD) != (mval & ~HYDRA_AUDIT_AD)) {
				audit_fail(c->mm, a, "PTE replica diverged", n,
					   mval & ~HYDRA_AUDIT_AD,
					   v & ~HYDRA_AUDIT_AD);
			}
			cur = READ_ONCE(cur->next_replica);
		}

		if (present)
			c->leaf_entries++;
	}
	spin_unlock(ptl);
}

static void audit_pud(struct audit_ctx *c, pud_t *pud, unsigned long pud_base,
		      int tree_node)
{
	pmd_t *pmd_arr = pmd_offset(pud, pud_base);
	struct page *master_pmd = virt_to_page(pmd_arr);
	spinlock_t *pmd_ptl;
	long members;
	int i;

	if (page_to_nid(master_pmd) != tree_node)
		audit_fail(c->mm, pud_base, "PMD table off node", tree_node,
			   tree_node, page_to_nid(master_pmd));

	members = audit_ring(c, master_pmd, HYDRA_PT_PMD, pud_base);
	c->pmd_pages++;
	c->pmd_members += members;

	pmd_ptl = pmd_lockptr(c->mm, pmd_arr);

	spin_lock(pmd_ptl);
	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd_t mpmd = READ_ONCE(pmd_arr[i]);
		unsigned long mval = pmd_val(mpmd);
		unsigned long off = (unsigned long)i * sizeof(pmd_t);
		unsigned long a = pud_base + ((unsigned long)i << PMD_SHIFT);
		bool none = pmd_none(mpmd);
		bool present = !none && pmd_present(mpmd);
		bool huge = present && (pmd_trans_huge(mpmd) || pmd_leaf(mpmd));
		bool expect_zero = none || !present;
		struct page *cur, *start;

		if (!huge && !expect_zero)
			continue;

		start = master_pmd;
		cur = READ_ONCE(master_pmd->next_replica);
		while (cur && cur != start) {
			unsigned long v =
				READ_ONCE(*(unsigned long *)(page_address(cur) + off));
			int n = page_to_nid(cur);

			if (expect_zero) {
				if (v != 0)
					audit_fail(c->mm, a,
						   "PMD replica present, master absent",
						   n, 0, v);
			} else if (v != 0 &&
				   (v & ~HYDRA_AUDIT_AD) != (mval & ~HYDRA_AUDIT_AD)) {
				audit_fail(c->mm, a, "PMD huge replica diverged",
					   n, mval & ~HYDRA_AUDIT_AD,
					   v & ~HYDRA_AUDIT_AD);
			}
			cur = READ_ONCE(cur->next_replica);
		}

		if (huge)
			c->leaf_entries++;
	}
	spin_unlock(pmd_ptl);

	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd_t mpmd = READ_ONCE(pmd_arr[i]);
		unsigned long mval = pmd_val(mpmd);
		unsigned long off = (unsigned long)i * sizeof(pmd_t);
		unsigned long a = pud_base + ((unsigned long)i << PMD_SHIFT);
		bool none = pmd_none(mpmd);
		bool present = !none && pmd_present(mpmd);
		bool huge = present && (pmd_trans_huge(mpmd) || pmd_leaf(mpmd));
		bool bad = present && !huge && pmd_bad(mpmd);
		bool table = present && !huge && !bad;
		unsigned long master_flags = mval & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD;
		struct page *master_child, *cur, *start;

		if (bad)
			audit_fail(c->mm, a, "PMD master entry bad", tree_node,
				   0, mval);
		if (!table)
			continue;

		master_child = pfn_to_page((mval & PTE_PFN_MASK) >> PAGE_SHIFT);

		start = master_pmd;
		cur = READ_ONCE(master_pmd->next_replica);
		while (cur && cur != start) {
			unsigned long v =
				READ_ONCE(*(unsigned long *)(page_address(cur) + off));
			int n = page_to_nid(cur);
			pmd_t vpmd = __pmd(v);
			struct page *exp;
			unsigned long want;

			if (v == 0) {
				cur = READ_ONCE(cur->next_replica);
				continue;
			}
			if (pmd_trans_huge(vpmd) || pmd_leaf(vpmd))
				audit_fail(c->mm, a, "PMD master table but replica huge",
					   n, 0, v);
			if ((v & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD) != master_flags)
				audit_fail(c->mm, a, "PMD table flag mismatch",
					   n, master_flags,
					   v & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD);
			exp = audit_member_on_node(master_child, n);
			if (!exp)
				audit_fail(c->mm, a, "PMD table child not on node",
					   n, 0, 0);
			want = (unsigned long)page_to_pfn(exp) << PAGE_SHIFT;
			if ((v & PTE_PFN_MASK) != want)
				audit_fail(c->mm, a, "PMD table wrong child",
					   n, want, v & PTE_PFN_MASK);
			cur = READ_ONCE(cur->next_replica);
		}

		c->table_entries++;
		audit_pte_page(c, master_child, a, tree_node, &pmd_arr[i]);
	}
}

static void audit_walk(struct audit_ctx *c)
{
	struct mm_struct *mm = c->mm;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);
	unsigned long done_pud = 0;

	{
		VMA_ITERATOR(vmi2, mm, 0);
		struct vm_area_struct *v;
		unsigned long claim_pud = 0;
		int claim_master = -1;
		bool have_claim = false;

		for_each_vma(vmi2, v) {
			int m = v->master_pgd_node;
			unsigned long p;

			for (p = v->vm_start & PUD_MASK; p < v->vm_end;
			     p += PUD_SIZE) {
				if (!have_claim || p != claim_pud) {
					claim_pud = p;
					claim_master = m;
					have_claim = true;
				} else if (m != claim_master) {
					audit_fail(mm, p,
						   "two VMAs share a PUD with different master",
						   claim_master, claim_master, m);
				}
			}
		}
	}

	rcu_read_lock();
	for_each_vma(vmi, vma) {
		int master = vma->master_pgd_node;
		pgd_t *root;
		int tree_node;
		unsigned long pud_base;

		if (master < 0 || master >= NUMA_NODE_COUNT)
			continue;
		root = mm->repl_pgd[master];
		if (!root)
			continue;
		tree_node = page_to_nid(virt_to_page(root));

		for (pud_base = vma->vm_start & PUD_MASK;
		     pud_base < vma->vm_end;
		     pud_base += PUD_SIZE) {
			pgd_t *pgd;
			p4d_t *p4d;
			pud_t *pud;

			if (pud_base < done_pud)
				continue;
			done_pud = pud_base + PUD_SIZE;

			pgd = pgd_offset_pgd(root, pud_base);
			if (pgd_none(*pgd) || pgd_bad(*pgd))
				continue;

			p4d = p4d_offset(pgd, pud_base);
			if (virt_to_page(p4d) != virt_to_page(root) &&
			    page_to_nid(virt_to_page(p4d)) != tree_node)
				audit_fail(mm, pud_base, "P4D table off node",
					   tree_node, tree_node,
					   page_to_nid(virt_to_page(p4d)));
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				continue;

			pud = pud_offset(p4d, pud_base);
			if (page_to_nid(virt_to_page(pud)) != tree_node)
				audit_fail(mm, pud_base, "PUD table off node",
					   tree_node, tree_node,
					   page_to_nid(virt_to_page(pud)));
			if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
				continue;

			audit_pud(c, pud, pud_base, tree_node);
		}
	}
	rcu_read_unlock();
}

int hydra_audit_run(pid_t pid)
{
	struct task_struct *task = NULL;
	struct mm_struct *mm;
	struct audit_ctx c;
	struct hydra_audit_result res;
	int target_pid, i, cnt;

	if (pid == 0) {
		mm = current->mm;
		if (mm)
			mmget(mm);
		target_pid = current->tgid;
	} else {
		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (!task)
			return -ESRCH;
		mm = get_task_mm(task);
		target_pid = pid;
	}

	if (!mm) {
		if (task)
			put_task_struct(task);
		return -EINVAL;
	}

	memset(&res, 0, sizeof(res));
	res.valid = 1;
	res.pid = target_pid;

	if (READ_ONCE(mm->lazy_repl_enabled)) {
		memset(&c, 0, sizeof(c));
		c.mm = mm;

		mmap_write_lock(mm);
		audit_walk(&c);
		mmap_write_unlock(mm);

		res.enabled = 1;
		cnt = 1;
		for (i = 0; i < NUMA_NODE_COUNT; i++)
			if (mm->repl_pgd[i] && mm->repl_pgd[i] != mm->pgd)
				cnt++;
		res.nodes = cnt;
		res.pmd_pages = c.pmd_pages;
		res.pmd_members = c.pmd_members;
		res.pte_pages = c.pte_pages;
		res.pte_members = c.pte_members;
		res.leaf_entries = c.leaf_entries;
		res.table_entries = c.table_entries;
	}

	spin_lock(&hydra_audit_lock);
	hydra_audit_last = res;
	spin_unlock(&hydra_audit_lock);

	mmput(mm);
	if (task)
		put_task_struct(task);
	return 0;
}

void hydra_audit_seq_show(struct seq_file *m)
{
	struct hydra_audit_result r;

	spin_lock(&hydra_audit_lock);
	r = hydra_audit_last;
	spin_unlock(&hydra_audit_lock);

	if (!r.valid) {
		seq_puts(m, "  no audit run yet (write a pid to trigger; 0 = writer's own mm)\n");
		return;
	}

	seq_puts(m, "  Hydra page-table coherence audit  (last run)\n");
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-28s %d\n", "target pid", r.pid);
	seq_printf(m, "    %-28s %s\n", "replication enabled",
		   r.enabled ? "yes" : "no");
	if (!r.enabled) {
		seq_puts(m, "    (nothing to audit)\n");
		return;
	}
	seq_printf(m, "    %-28s %d\n", "replica nodes", r.nodes);
	seq_printf(m, "    %-28s %s\n", "result",
		   "CLEAN (a violation BUGs the kernel)");

	seq_puts(m, "\n  rows = chained level,  cols = master pages walked vs ring members\n");
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-6s %16s %16s\n", "level", "master pages", "ring members");
	seq_printf(m, "    %-6s %16ld %16ld\n", "PMD", r.pmd_pages, r.pmd_members);
	seq_printf(m, "    %-6s %16ld %16ld\n", "PTE", r.pte_pages, r.pte_members);
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-28s %ld\n", "leaf entries checked", r.leaf_entries);
	seq_printf(m, "    %-28s %ld\n", "table (->PTE) entries", r.table_entries);
	seq_puts(m, "    note: ring members >= master pages because each replicated page\n");
	seq_puts(m, "          carries one ring entry per faulted node; an absent (un-faulted)\n");
	seq_puts(m, "          replica is legal under Hydra lazy/partial replication.\n");
}

struct hydra_sweep_result {
	int valid;
	int pid;
	int enabled;
	long pages[NUMA_NODE_COUNT][HYDRA_PT_NR_LEVELS];
	long leaf_checked;
	long table_checked;
	long lazy_absent;
	long master_leaves;
	long gap_tables;
};

static DEFINE_SPINLOCK(hydra_sweep_lock);
static struct hydra_sweep_result hydra_sweep_last;

struct hydra_sweep_ctx {
	struct mm_struct *mm;
	int node;
	struct vm_area_struct *leaf_vma;
	struct hydra_sweep_result *res;
};

static bool sweep_leaf_mapped(struct hydra_sweep_ctx *c, unsigned long addr)
{
	if (c->leaf_vma && addr >= c->leaf_vma->vm_start &&
	    addr < c->leaf_vma->vm_end)
		return true;
	c->leaf_vma = find_vma(c->mm, addr);
	return c->leaf_vma && c->leaf_vma->vm_start <= addr;
}

static void sweep_check_nid(struct hydra_sweep_ctx *c, void *table,
			    const char *what, unsigned long addr)
{
	int nid = page_to_nid(virt_to_page(table));

	if (nid != c->node)
		audit_fail(c->mm, addr, what, c->node, c->node, nid);
}

static int sweep_pud_master(struct mm_struct *mm, unsigned long pud_base)
{
	struct vm_area_struct *vma = find_vma(mm, pud_base);

	if (!vma || vma->vm_start >= pud_base + PUD_SIZE)
		return -1;
	return vma->master_pgd_node;
}

static void sweep_pte_local(struct hydra_sweep_ctx *c, pte_t *base,
			    unsigned long addr_base, bool gap)
{
	int i;

	for (i = 0; i < PTRS_PER_PTE; i++) {
		unsigned long v = READ_ONCE(((unsigned long *)base)[i]);
		unsigned long a = addr_base + ((unsigned long)i << PAGE_SHIFT);

		if (v == 0 || !pte_present(__pte(v)))
			continue;
		if (gap || !sweep_leaf_mapped(c, a))
			audit_fail(c->mm, a, "sweep PTE leaf outside any VMA",
				   c->node, 0, v);
		c->res->master_leaves++;
	}
}

static void sweep_pmd_local(struct hydra_sweep_ctx *c, pmd_t *base,
			    unsigned long pud_base, bool gap)
{
	int i;

	if (gap)
		c->res->gap_tables++;

	for (i = 0; i < PTRS_PER_PMD; i++) {
		unsigned long v = READ_ONCE(((unsigned long *)base)[i]);
		unsigned long a = pud_base + ((unsigned long)i << PMD_SHIFT);
		pmd_t p = __pmd(v);
		pte_t *pte;

		if (v == 0 || !pmd_present(p))
			continue;
		if (pmd_trans_huge(p) || pmd_leaf(p)) {
			if (gap || !sweep_leaf_mapped(c, a))
				audit_fail(c->mm, a,
					   "sweep PMD leaf outside any VMA",
					   c->node, 0, v);
			c->res->master_leaves++;
			continue;
		}
		if (pmd_bad(p))
			audit_fail(c->mm, a, "sweep PMD entry bad",
				   c->node, 0, v);

		pte = (pte_t *)__va(v & PTE_PFN_MASK);
		sweep_check_nid(c, pte, "sweep PTE table off node", a);
		c->res->pages[c->node][HYDRA_PT_PTE]++;
		if (gap)
			c->res->gap_tables++;
		sweep_pte_local(c, pte, a, gap);
	}
}

static void sweep_pte_cross(struct hydra_sweep_ctx *c, pte_t *r_base,
			    pte_t *m_base, pmd_t *m_pmd, unsigned long addr_base)
{
	spinlock_t *ptl = pte_lockptr(c->mm, m_pmd);
	int i;

	spin_lock(ptl);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		unsigned long rv = READ_ONCE(((unsigned long *)r_base)[i]);
		unsigned long mv = READ_ONCE(((unsigned long *)m_base)[i]);
		unsigned long a = addr_base + ((unsigned long)i << PAGE_SHIFT);

		if (rv == 0) {
			if (mv != 0 && pte_present(__pte(mv)))
				c->res->lazy_absent++;
			continue;
		}
		if (!pte_present(__pte(rv)))
			audit_fail(c->mm, a, "sweep replica PTE not present",
				   c->node, 0, rv);
		if (!sweep_leaf_mapped(c, a))
			audit_fail(c->mm, a, "sweep PTE leaf outside any VMA",
				   c->node, 0, rv);
		if (mv == 0 || !pte_present(__pte(mv)))
			audit_fail(c->mm, a,
				   "sweep replica PTE present, master absent",
				   c->node, mv, rv);
		if ((rv ^ mv) & ~HYDRA_AUDIT_AD)
			audit_fail(c->mm, a, "sweep replica PTE diverged",
				   c->node, mv & ~HYDRA_AUDIT_AD,
				   rv & ~HYDRA_AUDIT_AD);
		c->res->leaf_checked++;
	}
	spin_unlock(ptl);
}

static void sweep_pmd_cross(struct hydra_sweep_ctx *c, pud_t *pud,
			    pmd_t *r_base, pmd_t *m_base,
			    unsigned long pud_base)
{
	spinlock_t *pmd_ptl;
	spinlock_t *pud_ptl;
	int i;

	pud_ptl = pud_lock(c->mm, pud);
	if (audit_member_on_node(virt_to_page(m_base), c->node) !=
	    virt_to_page(r_base)) {
		spin_unlock(pud_ptl);
		audit_fail(c->mm, pud_base,
			   "sweep replica PMD page not in master ring",
			   c->node, (unsigned long)virt_to_page(m_base),
			   (unsigned long)virt_to_page(r_base));
	}
	spin_unlock(pud_ptl);

	pmd_ptl = pmd_lockptr(c->mm, m_base);
	spin_lock(pmd_ptl);
	for (i = 0; i < PTRS_PER_PMD; i++) {
		unsigned long rv = READ_ONCE(((unsigned long *)r_base)[i]);
		unsigned long mv = READ_ONCE(((unsigned long *)m_base)[i]);
		unsigned long a = pud_base + ((unsigned long)i << PMD_SHIFT);
		pmd_t r = __pmd(rv);
		pmd_t mp = __pmd(mv);

		if (rv == 0) {
			if (mv != 0 && pmd_present(mp))
				c->res->lazy_absent++;
			continue;
		}
		if (!pmd_present(r))
			audit_fail(c->mm, a, "sweep replica PMD not present",
				   c->node, 0, rv);
		if (pmd_trans_huge(r) || pmd_leaf(r)) {
			if (!sweep_leaf_mapped(c, a))
				audit_fail(c->mm, a,
					   "sweep PMD leaf outside any VMA",
					   c->node, 0, rv);
			if (!pmd_present(mp) ||
			    !(pmd_trans_huge(mp) || pmd_leaf(mp)))
				audit_fail(c->mm, a,
					   "sweep replica huge PMD, master not huge",
					   c->node, mv, rv);
			if ((rv ^ mv) & ~HYDRA_AUDIT_AD)
				audit_fail(c->mm, a,
					   "sweep replica huge PMD diverged",
					   c->node, mv & ~HYDRA_AUDIT_AD,
					   rv & ~HYDRA_AUDIT_AD);
			c->res->leaf_checked++;
			continue;
		}
		if (pmd_bad(r))
			audit_fail(c->mm, a, "sweep replica PMD entry bad",
				   c->node, 0, rv);
		if (!pmd_present(mp) || pmd_trans_huge(mp) || pmd_leaf(mp) ||
		    pmd_bad(mp))
			audit_fail(c->mm, a,
				   "sweep replica PMD table, master slot not table",
				   c->node, mv, rv);
		if ((rv & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD) !=
		    (mv & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD))
			audit_fail(c->mm, a, "sweep PMD table flag mismatch",
				   c->node, mv & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD,
				   rv & ~PTE_PFN_MASK & ~HYDRA_AUDIT_AD);
		if (audit_member_on_node(
			    pfn_to_page((mv & PTE_PFN_MASK) >> PAGE_SHIFT),
			    c->node) !=
		    pfn_to_page((rv & PTE_PFN_MASK) >> PAGE_SHIFT))
			audit_fail(c->mm, a,
				   "sweep replica PTE page not in master ring",
				   c->node, mv & PTE_PFN_MASK,
				   rv & PTE_PFN_MASK);
		c->res->table_checked++;
	}
	spin_unlock(pmd_ptl);

	for (i = 0; i < PTRS_PER_PMD; i++) {
		unsigned long rv = READ_ONCE(((unsigned long *)r_base)[i]);
		unsigned long mv = READ_ONCE(((unsigned long *)m_base)[i]);
		unsigned long a = pud_base + ((unsigned long)i << PMD_SHIFT);
		pmd_t r = __pmd(rv);
		pmd_t mp = __pmd(mv);

		if (rv == 0 || !pmd_present(r) || pmd_trans_huge(r) ||
		    pmd_leaf(r) || pmd_bad(r))
			continue;
		if (mv == 0 || !pmd_present(mp) || pmd_trans_huge(mp) ||
		    pmd_leaf(mp) || pmd_bad(mp))
			continue;
		c->res->pages[c->node][HYDRA_PT_PTE]++;
		sweep_pte_cross(c, (pte_t *)__va(rv & PTE_PFN_MASK),
				(pte_t *)__va(mv & PTE_PFN_MASK),
				&m_base[i], a);
	}
}

static void sweep_tree(struct hydra_sweep_ctx *c, pgd_t *root)
{
	struct mm_struct *mm = c->mm;
	unsigned long addr = 0, end = mm->task_size;
	unsigned long next_pgd, next_p4d, next_pud;
	pgd_t *pgd;

	if (!end)
		return;

	sweep_check_nid(c, root, "sweep PGD root off node", 0);
	c->res->pages[c->node][HYDRA_PT_PGD]++;

	pgd = pgd_offset_pgd(root, addr);
	do {
		p4d_t *p4d;

		next_pgd = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			goto next_pgd_sw;

		p4d = p4d_offset(pgd, addr);
		if (virt_to_page(p4d) != virt_to_page(root)) {
			sweep_check_nid(c, p4d, "sweep P4D table off node",
					addr);
			c->res->pages[c->node][HYDRA_PT_P4D]++;
		}
		do {
			pud_t *pud;

			next_p4d = p4d_addr_end(addr, next_pgd);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				goto next_p4d_sw;

			pud = pud_offset(p4d, addr);
			sweep_check_nid(c, pud, "sweep PUD table off node",
					addr);
			c->res->pages[c->node][HYDRA_PT_PUD]++;
			do {
				unsigned long pud_base;
				pmd_t *pmd;
				int master;

				next_pud = pud_addr_end(addr, next_p4d);
				if (pud_none(*pud) || pud_bad(*pud) ||
				    pud_leaf(*pud))
					goto next_pud_sw;

				pud_base = addr & PUD_MASK;
				pmd = pmd_offset(pud, pud_base);
				sweep_check_nid(c, pmd,
						"sweep PMD table off node",
						pud_base);
				c->res->pages[c->node][HYDRA_PT_PMD]++;

				master = sweep_pud_master(mm, pud_base);
				if (master == c->node) {
					sweep_pmd_local(c, pmd, pud_base,
							false);
				} else if (master < 0) {
					sweep_pmd_local(c, pmd, pud_base,
							true);
				} else {
					pmd_t *m_pmd = hydra_walk_to_pmd(
							mm, pud_base, master);

					if (HYDRA_WALK_BAD(m_pmd))
						audit_fail(mm, pud_base,
							   "sweep replica PMD table without master table",
							   c->node, master, 0);
					sweep_pmd_cross(c, pud, pmd,
						(pmd_t *)((unsigned long)m_pmd & PAGE_MASK),
						pud_base);
				}
next_pud_sw:
				addr = next_pud;
			} while (pud++, addr != next_p4d);
next_p4d_sw:
			addr = next_p4d;
		} while (p4d++, addr != next_pgd);
next_pgd_sw:
		addr = next_pgd;
	} while (pgd++, addr != end);
}

int hydra_sweep_run(pid_t pid)
{
	struct task_struct *task = NULL;
	struct mm_struct *mm;
	struct hydra_sweep_ctx c;
	struct hydra_sweep_result res;
	int target_pid, node;

	if (pid == 0) {
		mm = current->mm;
		if (mm)
			mmget(mm);
		target_pid = current->tgid;
	} else {
		rcu_read_lock();
		task = find_task_by_vpid(pid);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (!task)
			return -ESRCH;
		mm = get_task_mm(task);
		target_pid = pid;
	}

	if (!mm) {
		if (task)
			put_task_struct(task);
		return -EINVAL;
	}

	memset(&res, 0, sizeof(res));
	res.valid = 1;
	res.pid = target_pid;

	if (READ_ONCE(mm->lazy_repl_enabled)) {
		res.enabled = 1;
		memset(&c, 0, sizeof(c));
		c.mm = mm;
		c.res = &res;

		mmap_write_lock(mm);
		rcu_read_lock();
		for (node = 0; node < NUMA_NODE_COUNT; node++) {
			if (!mm->repl_pgd[node])
				continue;
			c.node = node;
			c.leaf_vma = NULL;
			sweep_tree(&c, mm->repl_pgd[node]);
		}
		rcu_read_unlock();
		mmap_write_unlock(mm);
	}

	spin_lock(&hydra_sweep_lock);
	hydra_sweep_last = res;
	spin_unlock(&hydra_sweep_lock);

	mmput(mm);
	if (task)
		put_task_struct(task);
	return 0;
}

void hydra_sweep_seq_show(struct seq_file *m)
{
	static const char * const lvl[HYDRA_PT_NR_LEVELS] = {
		[HYDRA_PT_PGD] = "PGD",
		[HYDRA_PT_P4D] = "P4D",
		[HYDRA_PT_PUD] = "PUD",
		[HYDRA_PT_PMD] = "PMD",
		[HYDRA_PT_PTE] = "PTE",
	};
	struct hydra_sweep_result r;
	char buf[12];
	int node, level;

	spin_lock(&hydra_sweep_lock);
	r = hydra_sweep_last;
	spin_unlock(&hydra_sweep_lock);

	if (!r.valid) {
		seq_puts(m, "  no sweep run yet (write a pid to trigger; 0 = writer's own mm)\n");
		return;
	}

	seq_puts(m, "  Hydra full-tree sweep  (last run)\n");
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-32s %d\n", "target pid", r.pid);
	seq_printf(m, "    %-32s %s\n", "replication enabled",
		   r.enabled ? "yes" : "no");
	if (!r.enabled) {
		seq_puts(m, "    (nothing to sweep)\n");
		return;
	}
	seq_printf(m, "    %-32s %s\n", "result",
		   "CLEAN (a violation BUGs the kernel)");

	seq_puts(m, "\n  table pages visited  [rows = level, cols = per-node tree]\n");
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-6s", "level");
	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		scnprintf(buf, sizeof(buf), "n%d", node);
		seq_printf(m, " %7s", buf);
	}
	seq_putc(m, '\n');
	for (level = 0; level < HYDRA_PT_NR_LEVELS; level++) {
		seq_printf(m, "    %-6s", lvl[level]);
		for (node = 0; node < NUMA_NODE_COUNT; node++)
			seq_printf(m, " %7ld", r.pages[node][level]);
		seq_putc(m, '\n');
	}
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-32s %12ld\n", "replica leaves checked vs master",
		   r.leaf_checked);
	seq_printf(m, "    %-32s %12ld\n", "replica table entries verified",
		   r.table_checked);
	seq_printf(m, "    %-32s %12ld\n", "lazily absent replica slots",
		   r.lazy_absent);
	seq_printf(m, "    %-32s %12ld\n", "master-tree leaves range-checked",
		   r.master_leaves);
	seq_printf(m, "    %-32s %12ld\n", "table pages in unmapped gaps",
		   r.gap_tables);
}

static DEFINE_SPINLOCK(hydra_walk_lock);
static pid_t hydra_walk_pid;
static unsigned long hydra_walk_va;

int hydra_walk_set(pid_t pid, unsigned long addr)
{
	if (pid < 0)
		return -EINVAL;
	if (pid == 0)
		pid = current->tgid;

	spin_lock(&hydra_walk_lock);
	hydra_walk_pid = pid;
	hydra_walk_va = addr;
	spin_unlock(&hydra_walk_lock);
	return 0;
}

static void walk_flags(struct seq_file *m, unsigned long v)
{
	seq_printf(m, "%s%s%s%s%s%s%s%s%s",
		   (v & _PAGE_PRESENT) ? " P" : "",
		   (v & _PAGE_RW) ? " W" : "",
		   (v & _PAGE_USER) ? " U" : "",
		   (v & _PAGE_ACCESSED) ? " A" : "",
		   (v & _PAGE_DIRTY) ? " D" : "",
		   (v & _PAGE_SAVED_DIRTY) ? " SD" : "",
		   (v & _PAGE_PSE) ? " PSE" : "",
		   (v & _PAGE_GLOBAL) ? " G" : "",
		   (v & _PAGE_NX) ? " NX" : "");
}

static void walk_ring(struct seq_file *m, struct mm_struct *mm,
		      struct page *pg)
{
	struct page *cur;
	bool owner_bad;
	int cnt = 1;

	cur = READ_ONCE(pg->next_replica);
	if (!cur) {
		seq_puts(m, "      ring: none (unchained)\n");
		return;
	}

	owner_bad = pg->pt_owner_mm != mm;
	seq_printf(m, "      ring: n%d", page_to_nid(pg));
	while (cur && cur != pg && cnt <= NUMA_NODE_COUNT) {
		seq_printf(m, " -> n%d", page_to_nid(cur));
		if (cur->pt_owner_mm != mm)
			owner_bad = true;
		cur = READ_ONCE(cur->next_replica);
		cnt++;
	}
	seq_printf(m, "  (%s, %d members%s)\n",
		   cur == pg ? "closed" : "OPEN",
		   cnt, owner_bad ? ", OWNER MISMATCH" : "");
}

static void walk_child_nid(struct seq_file *m, unsigned long v)
{
	unsigned long pfn = (v & PTE_PFN_MASK) >> PAGE_SHIFT;

	if (pfn_valid(pfn))
		seq_printf(m, "  -> child nid %d",
			   page_to_nid(pfn_to_page(pfn)));
	else
		seq_puts(m, "  -> child pfn INVALID");
}

static void hydra_walk_tree(struct seq_file *m, struct mm_struct *mm,
			    pgd_t *root, int node, unsigned long va)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	unsigned long v;

	seq_printf(m, "\n  node %d  root %px (page nid %d)%s\n",
		   node, root, page_to_nid(virt_to_page(root)),
		   root == mm->pgd ? "  = mm->pgd" : "");

	pgdp = pgd_offset_pgd(root, va);
	v = pgd_val(READ_ONCE(*pgdp));
	seq_printf(m, "    PGD  idx %3lu  val %016lx", pgd_index(va), v);
	if (!(v & _PAGE_PRESENT)) {
		seq_puts(m, "  (none)\n");
		return;
	}
	walk_child_nid(m, v);
	seq_putc(m, '\n');

	p4dp = p4d_offset(pgdp, va);
	if (pgtable_l5_enabled()) {
		v = p4d_val(READ_ONCE(*p4dp));
		seq_printf(m, "    P4D  idx %3lu  val %016lx",
			   p4d_index(va), v);
		if (!(v & _PAGE_PRESENT)) {
			seq_puts(m, "  (none)\n");
			return;
		}
		walk_child_nid(m, v);
		seq_putc(m, '\n');
	}

	pudp = pud_offset(p4dp, va);
	v = pud_val(READ_ONCE(*pudp));
	seq_printf(m, "    PUD  idx %3lu  val %016lx  [table nid %d]",
		   pud_index(va), v, page_to_nid(virt_to_page(pudp)));
	if (!(v & _PAGE_PRESENT)) {
		seq_puts(m, "  (none)\n");
		return;
	}
	if (pud_leaf(__pud(v))) {
		seq_printf(m, "  1G leaf pfn %lx",
			   (v & PTE_PFN_MASK) >> PAGE_SHIFT);
		walk_flags(m, v);
		seq_putc(m, '\n');
		return;
	}
	walk_child_nid(m, v);
	seq_putc(m, '\n');

	pmdp = pmd_offset(pudp, va);
	v = pmd_val(READ_ONCE(*pmdp));
	seq_printf(m, "    PMD  idx %3lu  val %016lx  [table nid %d]",
		   pmd_index(va), v, page_to_nid(virt_to_page(pmdp)));
	if (v == 0) {
		seq_puts(m, "  (none)\n");
		walk_ring(m, mm, virt_to_page(pmdp));
		return;
	}
	if (!pmd_present(__pmd(v))) {
		seq_puts(m, "  (not present)\n");
		walk_ring(m, mm, virt_to_page(pmdp));
		return;
	}
	if (pmd_trans_huge(__pmd(v)) || pmd_leaf(__pmd(v))) {
		seq_printf(m, "  2M leaf pfn %lx",
			   (v & PTE_PFN_MASK) >> PAGE_SHIFT);
		walk_flags(m, v);
		seq_putc(m, '\n');
		walk_ring(m, mm, virt_to_page(pmdp));
		return;
	}
	walk_child_nid(m, v);
	seq_putc(m, '\n');
	walk_ring(m, mm, virt_to_page(pmdp));
	if (pmd_bad(__pmd(v))) {
		seq_puts(m, "      (PMD entry bad, stopping)\n");
		return;
	}

	ptep = pte_offset_kernel(pmdp, va);
	v = pte_val(READ_ONCE(*ptep));
	seq_printf(m, "    PTE  idx %3lu  val %016lx  [table nid %d]",
		   pte_index(va), v, page_to_nid(virt_to_page(ptep)));
	if (v == 0) {
		seq_puts(m, "  (none)");
	} else if (!pte_present(__pte(v))) {
		seq_puts(m, "  (not present)");
	} else {
		seq_printf(m, "  pfn %lx", (v & PTE_PFN_MASK) >> PAGE_SHIFT);
		walk_flags(m, v);
	}
	seq_putc(m, '\n');
	walk_ring(m, mm, virt_to_page(ptep));
}

void hydra_walk_seq_show(struct seq_file *m)
{
	struct task_struct *task;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long va;
	pid_t pid;
	int node;

	spin_lock(&hydra_walk_lock);
	pid = hydra_walk_pid;
	va = hydra_walk_va;
	spin_unlock(&hydra_walk_lock);

	if (!pid) {
		seq_puts(m, "  no walk target set (write \"<pid> <hexaddr>\"; pid 0 = writer)\n");
		return;
	}

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task) {
		seq_printf(m, "  target pid %d not found\n", pid);
		return;
	}
	mm = get_task_mm(task);
	if (!mm) {
		put_task_struct(task);
		seq_printf(m, "  target pid %d has no mm\n", pid);
		return;
	}

	seq_puts(m, "  Hydra per-node page-table walk\n");
	seq_puts(m, "  ------------------------------------------------------------------\n");
	seq_printf(m, "    %-16s %d\n", "target pid", pid);
	seq_printf(m, "    %-16s %016lx\n", "address", va);
	seq_printf(m, "    %-16s %s\n", "replication",
		   READ_ONCE(mm->lazy_repl_enabled) ? "enabled" : "disabled");

	mmap_read_lock(mm);
	if (va >= mm->task_size) {
		seq_printf(m, "    %-16s %s\n", "vma",
			   "address beyond task size");
		goto out;
	}
	vma = vma_lookup(mm, va);
	if (vma)
		seq_printf(m, "    %-16s %016lx-%016lx  master node %lu\n",
			   "vma", vma->vm_start, vma->vm_end,
			   vma->master_pgd_node);
	else
		seq_printf(m, "    %-16s %s\n", "vma",
			   "none (address not mapped by any VMA)");

	rcu_read_lock();
	if (!READ_ONCE(mm->lazy_repl_enabled)) {
		hydra_walk_tree(m, mm, mm->pgd,
				page_to_nid(virt_to_page(mm->pgd)), va);
	} else {
		for (node = 0; node < NUMA_NODE_COUNT; node++) {
			if (!mm->repl_pgd[node])
				continue;
			hydra_walk_tree(m, mm, mm->repl_pgd[node], node, va);
		}
	}
	rcu_read_unlock();
out:
	mmap_read_unlock(mm);
	mmput(mm);
	put_task_struct(task);
}
