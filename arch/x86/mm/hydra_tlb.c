#include <linux/mm.h>
#include <linux/hydra.h>
#include <linux/interval_tree.h>
#include <linux/spinlock.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/tlb.h>

int sysctl_hydra_tlbflush_nested_opt __read_mostly = 1;
int sysctl_hydra_tlbflush_coalesce __read_mostly = 1;

bool hydra_flush_coalesce_enter(struct mm_struct *mm, u64 my_gen)
{
	spin_lock(&mm->hydra_flush_lock);
	mm->hydra_flush_pool_ops++;
	for (;;) {
		if (mm->hydra_flush_done_gen >= my_gen) {
			spin_unlock(&mm->hydra_flush_lock);
			return true;
		}
		if (!mm->hydra_flush_inflight) {
			mm->hydra_flush_inflight = true;
			spin_unlock(&mm->hydra_flush_lock);
			return false;
		}
		spin_unlock(&mm->hydra_flush_lock);
		while (READ_ONCE(mm->hydra_flush_inflight))
			cpu_relax();
		spin_lock(&mm->hydra_flush_lock);
	}
}

void hydra_flush_coalesce_leader_done(struct mm_struct *mm, u64 covered_gen)
{
	spin_lock(&mm->hydra_flush_lock);
	if (covered_gen > mm->hydra_flush_done_gen)
		mm->hydra_flush_done_gen = covered_gen;
	mm->hydra_flush_degree = mm->hydra_flush_pool_ops ?
				 mm->hydra_flush_pool_ops : 1;
	mm->hydra_flush_pool_ops = 0;
	mm->hydra_flush_inflight = false;
	spin_unlock(&mm->hydra_flush_lock);
}

void hydra_tlb_register(struct mmu_gather *tlb, unsigned long start,
		       unsigned long end)
{
	struct mm_struct *mm = tlb->mm;
	unsigned long last;

	if (!READ_ONCE(sysctl_hydra_tlbflush_nested_opt) ||
	    !mm->lazy_repl_enabled || tlb->fullmm || end <= start)
		return;

	last = end - 1;

	spin_lock(&mm->hydra_tlb_lock);
	if (tlb->hydra_registered) {
		if (start < tlb->hydra_node.start || last > tlb->hydra_node.last) {
			interval_tree_remove(&tlb->hydra_node,
					     &mm->hydra_tlb_intervals);
			if (start < tlb->hydra_node.start)
				tlb->hydra_node.start = start;
			if (last > tlb->hydra_node.last)
				tlb->hydra_node.last = last;
			interval_tree_insert(&tlb->hydra_node,
					     &mm->hydra_tlb_intervals);
		}
	} else {
		tlb->hydra_node.start = start;
		tlb->hydra_node.last = last;
		interval_tree_insert(&tlb->hydra_node, &mm->hydra_tlb_intervals);
		mm->hydra_tlb_nr++;
		tlb->hydra_registered = 1;
	}
	spin_unlock(&mm->hydra_tlb_lock);
}

bool hydra_tlb_decide(struct mmu_gather *tlb, bool nested)
{
	struct mm_struct *mm = tlb->mm;
	struct interval_tree_node *n;
	bool force = nested;

	if (!tlb->hydra_registered)
		return nested;

	spin_lock(&mm->hydra_tlb_lock);

	if (nested && READ_ONCE(sysctl_hydra_tlbflush_nested_opt) &&
	    atomic_read(&mm->hydra_tlb_foreign) == 0) {
		force = false;
		if (mm->hydra_tlb_nr > 1) {
			for (n = interval_tree_iter_first(&mm->hydra_tlb_intervals,
							  tlb->hydra_node.start,
							  tlb->hydra_node.last);
			     n;
			     n = interval_tree_iter_next(n, tlb->hydra_node.start,
							 tlb->hydra_node.last)) {
				if (n != &tlb->hydra_node) {
					force = true;
					break;
				}
			}
		}
	}

	spin_unlock(&mm->hydra_tlb_lock);
	return force;
}

void hydra_tlb_unregister(struct mmu_gather *tlb)
{
	struct mm_struct *mm = tlb->mm;

	if (!tlb->hydra_registered)
		return;

	spin_lock(&mm->hydra_tlb_lock);
	interval_tree_remove(&tlb->hydra_node, &mm->hydra_tlb_intervals);
	mm->hydra_tlb_nr--;
	tlb->hydra_registered = 0;
	spin_unlock(&mm->hydra_tlb_lock);
}

void hydra_tlb_foreign_enter(struct mm_struct *mm)
{
	if (mm->lazy_repl_enabled)
		atomic_inc(&mm->hydra_tlb_foreign);
}

void hydra_tlb_foreign_exit(struct mm_struct *mm)
{
	if (mm->lazy_repl_enabled)
		atomic_dec(&mm->hydra_tlb_foreign);
}

void flush_tlb_vma_range(struct vm_area_struct *vma, unsigned long start,
			 unsigned long end, unsigned int stride_shift,
			 bool freed_tables)
{
	struct mm_struct *mm = vma->vm_mm;
	nodemask_t nodemask;
	int master_node;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (!mm->lazy_repl_enabled || !sysctl_hydra_tlbflush_opt)
		goto broadcast;

	master_node = vma->master_pgd_node;

	pgd = pgd_offset_node(mm, start, master_node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		goto broadcast;

	p4d = p4d_offset(pgd, start);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		goto broadcast;

	pud = pud_offset(p4d, start);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		goto broadcast;

	pmd = pmd_offset(pud, start);
	if (pmd_none(*pmd))
		goto broadcast;

	nodes_clear(nodemask);

	if (pmd_trans_huge(*pmd)) {
		if ((start & PUD_MASK) != ((end - 1) & PUD_MASK))
			goto broadcast;
		hydra_collect_repl_nodes(virt_to_page(pmd), &nodemask);
	} else {
		if (unlikely(pmd_bad(*pmd)))
			goto broadcast;
		if ((start & PMD_MASK) != ((end - 1) & PMD_MASK))
			goto broadcast;
		pte = pte_offset_kernel(pmd, start);
		hydra_collect_repl_nodes(virt_to_page(pte), &nodemask);
	}

	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, &nodemask);
	return;

broadcast:
	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, NULL);
}
