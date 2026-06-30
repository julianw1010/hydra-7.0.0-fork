#include <linux/mm.h>
#include <linux/atomic.h>
#include <linux/nodemask.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

atomic_long_t hydra_flush_stats[HFS_NR];
atomic_long_t hydra_flush_weight[NUMA_NODE_COUNT + 1];

atomic_long_t hydra_batched_pending_flush;
atomic_long_t hydra_ubc_set_pending;
atomic_long_t hydra_ubc_flush;

void flush_tlb_vma_range(struct vm_area_struct *vma, unsigned long start,
			 unsigned long end, unsigned int stride_shift,
			 bool freed_tables)
{
	struct mm_struct *mm = vma->vm_mm;
	nodemask_t nodemask;
	int master_node;
	int reason = HFS_BC_NOTREPL;
	int weight;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	atomic_long_inc(&hydra_flush_stats[HFS_TOTAL]);

	if (!mm->lazy_repl_enabled || !sysctl_hydra_tlbflush_opt) {
		reason = HFS_BC_NOTREPL;
		goto broadcast;
	}

	master_node = vma->master_pgd_node;

	pgd = pgd_offset_node(mm, start, master_node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
		reason = HFS_BC_PGD;
		goto broadcast;
	}

	p4d = p4d_offset(pgd, start);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
		reason = HFS_BC_P4D;
		goto broadcast;
	}

	pud = pud_offset(p4d, start);
	if (pud_none(*pud) || unlikely(pud_bad(*pud))) {
		reason = HFS_BC_PUD;
		goto broadcast;
	}

	pmd = pmd_offset(pud, start);
	if (pmd_none(*pmd)) {
		reason = HFS_BC_PMD_NONE;
		goto broadcast;
	}

	nodes_clear(nodemask);

	if (pmd_trans_huge(*pmd)) {
		if ((start & PUD_MASK) != ((end - 1) & PUD_MASK)) {
			reason = HFS_BC_RANGE_THP;
			goto broadcast;
		}
		hydra_collect_repl_nodes(virt_to_page(pmd), &nodemask);
		atomic_long_inc(&hydra_flush_stats[HFS_SCOPED_THP]);
	} else {
		if (unlikely(pmd_bad(*pmd))) {
			reason = HFS_BC_PMD_BAD;
			goto broadcast;
		}
		if ((start & PMD_MASK) != ((end - 1) & PMD_MASK)) {
			reason = HFS_BC_RANGE_PTE;
			goto broadcast;
		}
		pte = pte_offset_kernel(pmd, start);
		hydra_collect_repl_nodes(virt_to_page(pte), &nodemask);
		atomic_long_inc(&hydra_flush_stats[HFS_SCOPED_PTE]);
	}

	weight = nodes_weight(nodemask);
	if (weight < 0)
		weight = 0;
	if (weight > NUMA_NODE_COUNT)
		weight = NUMA_NODE_COUNT;
	atomic_long_inc(&hydra_flush_weight[weight]);

	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, &nodemask);
	return;

broadcast:
	atomic_long_inc(&hydra_flush_stats[reason]);
	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, NULL);
}
