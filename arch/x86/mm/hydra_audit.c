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

#define HYDRA_AUDIT_AD (_PAGE_ACCESSED | _PAGE_DIRTY)

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
