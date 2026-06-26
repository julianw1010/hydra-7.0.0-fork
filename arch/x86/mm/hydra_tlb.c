#include <linux/mm.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

void flush_tlb_vma_range(struct vm_area_struct *vma, unsigned long start,
			 unsigned long end, unsigned int stride_shift,
			 bool freed_tables)
{
	struct mm_struct *mm = vma->vm_mm;
	nodemask_t nodemask;
	unsigned long addr, next;
	int master_node;

	if (!mm->lazy_repl_enabled || !sysctl_hydra_tlbflush_opt) {
		flush_tlb_mm_node_range(mm, start, end, stride_shift,
					freed_tables, NULL);
		return;
	}

	if (sysctl_hydra_tlbflush_opt == 1 &&
	    ((start & PMD_MASK) != ((end - 1) & PMD_MASK))) {
		flush_tlb_mm_node_range(mm, start, end, stride_shift,
					freed_tables, NULL);
		return;
	}

	master_node = vma->master_pgd_node;
	nodes_clear(nodemask);

	addr = start;
	while (addr < end) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;

		next = (addr + PMD_SIZE) & PMD_MASK;
		if (next > end || next == 0)
			next = end;

		pgd = pgd_offset_node(mm, addr, master_node);
		if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))) {
			addr = next;
			continue;
		}

		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d))) {
			addr = next;
			continue;
		}

		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || unlikely(pud_bad(*pud))) {
			addr = next;
			continue;
		}

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd)) {
			addr = next;
			continue;
		}

		if (pmd_trans_huge(*pmd)) {
			if (hydra_calculate_tlbflush_nodemask(virt_to_page(pmd),
							     &nodemask)) {
				flush_tlb_mm_node_range(mm, start, end,
							stride_shift,
							freed_tables, NULL);
				return;
			}
			addr = next;
			continue;
		}

		if (unlikely(pmd_bad(*pmd))) {
			addr = next;
			continue;
		}

		pte = pte_offset_kernel(pmd, addr);
		if (hydra_calculate_tlbflush_nodemask(virt_to_page(pte),
						     &nodemask)) {
			flush_tlb_mm_node_range(mm, start, end, stride_shift,
						freed_tables, NULL);
			return;
		}

		addr = next;
	}

	if (nodes_empty(nodemask) && stride_shift >= PMD_SHIFT) {
		flush_tlb_mm_node_range(mm, start, end, stride_shift,
					freed_tables, NULL);
		return;
	}

	flush_tlb_mm_node_range(mm, start, end, stride_shift,
				freed_tables, &nodemask);
}
