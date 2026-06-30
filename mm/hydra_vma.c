#include "vma_internal.h"
#include "vma.h"

void hydra_vma_chown(struct vm_area_struct *vma, int node);

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
	int node, next_owner, cur_owner;
	struct vm_area_struct *cur;

	if (!mm->lazy_repl_enabled)
		return;

	cur = vma;
	while (cur) {
		int did_split = 0;

		node = hydra_lookup_pud_owner(mm, cur->vm_start, cur);
		if (node >= 0)
			hydra_vma_chown(cur, node);
		cur_owner = cur->master_pgd_node;

		pud_boundary = (cur->vm_start & PUD_MASK) + PUD_SIZE;
		while (pud_boundary < cur->vm_end) {
			next_owner = hydra_lookup_pud_owner(mm, pud_boundary, cur);
			if (next_owner < 0 || next_owner == cur_owner) {
				pud_boundary += PUD_SIZE;
				continue;
			}

			{
				VMA_ITERATOR(vmi, mm, pud_boundary);
				if (__split_vma(&vmi, cur, pud_boundary, 0)) {
					pr_emerg("HYDRA: fixup_pud_nodes cannot split VMA at %lx to separate PUD owners (OOM); one-master-per-PUD invariant would be violated\n",
						 pud_boundary);
					BUG();
				}
			}
			did_split = 1;
			break;
		}

		if (!did_split)
			break;

		cur = find_vma(mm, pud_boundary);
	}
}
