#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/hydra.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include "mm_internal.h"

#ifdef CONFIG_X86_PAE
#define PREALLOCATED_PMDS	PTRS_PER_PGD
#else
#define PREALLOCATED_PMDS	0
#endif

pgd_t *hydra_repl_pgd_alloc(struct mm_struct *mm, size_t nid)
{
	pgd_t *pgd;
	pmd_t *pmds[PREALLOCATED_PMDS];
	struct page *page;
	int order = pgd_allocation_order();
	nodemask_t nm = NODE_MASK_NONE;

	if (order == 0) {
		page = hydra_cache_pop(nid);
		if (page)
			goto got_page;
	}

	node_set(nid, nm);
	page = __alloc_pages(GFP_PGTABLE_USER | __GFP_THISNODE, order, nid, &nm);
	if (!page)
		goto out;

	page->next_replica = NULL;

got_page:
	page->pt_owner_mm = mm;
	pgd = (pgd_t *)page_address(page);

	if (PREALLOCATED_PMDS > 0) {
		if (preallocate_pmds(mm, pmds, PREALLOCATED_PMDS) != 0)
			goto out_free_page;
	}

	spin_lock(&pgd_lock);
	pgd_ctor(mm, pgd);
	pgd_prepopulate_pmd(mm, pgd, pmds);
	spin_unlock(&pgd_lock);

	page->pt_level = HYDRA_PT_PGD;
	hydra_pt_account(page, 1);

	return pgd;

out_free_page:
	page->pt_owner_mm = NULL;
	if (!hydra_try_return_page(page))
		free_pages((unsigned long)pgd, order);
out:
	return NULL;
}
