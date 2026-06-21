#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/page-flags.h>
#include <linux/hydra_util.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

struct page *repl_alloc_page_on_node(size_t nid, unsigned int order)
{
	nodemask_t nm = NODE_MASK_NONE;
	struct page *p;

	node_set(nid, nm);
	p = __alloc_pages((GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_THISNODE), order, nid, &nm);

	if (p) {
		p->next_replica = NULL;
		p->pt_owner_mm = NULL;
		p->mitosis_tracking = NULL;
	}

	return p;
}
