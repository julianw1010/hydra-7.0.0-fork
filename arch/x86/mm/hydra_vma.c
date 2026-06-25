#include "../../../mm/vma_internal.h"
#include "../../../mm/vma.h"

static int hydra_lookup_pud_owner(struct mm_struct *mm,
				  unsigned long addr,
				  struct vm_area_struct *exclude)
{
	unsigned long pud_start = addr & PUD_MASK;
	unsigned long pud_end = pud_start + PUD_SIZE;
	struct vm_area_struct *existing;
	VMA_ITERATOR(vmi, mm, pud_start);

	for_each_vma_range(vmi, existing, pud_end) {
		if (existing == exclude)
			continue;
		return existing->master_pgd_node;
	}

	return -1;
}

void hydra_fixup_pud_nodes(struct mm_struct *mm,
			   struct vm_area_struct *vma)
{
	unsigned long pud_boundary;
	int node;
	struct vm_area_struct *cur;

	if (!mm->lazy_repl_enabled)
		return;

	cur = vma;
	while (cur) {
		node = hydra_lookup_pud_owner(mm, cur->vm_start, cur);
		if (node >= 0)
			WRITE_ONCE(cur->master_pgd_node, node);

		pud_boundary = (cur->vm_start & PUD_MASK) + PUD_SIZE;
		if (pud_boundary >= cur->vm_end)
			break;

		{
			VMA_ITERATOR(vmi, mm, pud_boundary);
			if (split_vma(&vmi, cur, pud_boundary, 0))
				break;
		}

		cur = find_vma(mm, pud_boundary);
	}
}
