#ifndef _LINUX_HYDRA_UTIL_H
#define _LINUX_HYDRA_UTIL_H

#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/nodemask.h>
#include <linux/atomic.h>
#include <linux/highmem.h>
#include <linux/bitmap.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#define HYDRA_LEVEL_PTE 0
#define HYDRA_LEVEL_PMD 1

void hydra_reload_cr3(void *info);
int hydra_enable_replication(struct mm_struct *mm);
int hydra_repl_fault(struct vm_fault *vmf, int fault_node);

#define HYDRA_WALK_PGD_NONE ((void *)0x1)
#define HYDRA_WALK_P4D_NONE ((void *)0x11)
#define HYDRA_WALK_PUD_NONE ((void *)0x21)
#define HYDRA_WALK_PMD_NONE ((void *)0x31)

#define HYDRA_WALK_BAD(r) (((unsigned long)(r) & 1) == 1)

static inline pgd_t *hydra_pgd_offset(struct mm_struct *mm,
				       unsigned long address,
				       unsigned long node)
{
	if (mm->lazy_repl_enabled)
		return pgd_offset_node(mm, address, node);
	return pgd_offset(mm, address);
}

static inline pmd_t *hydra_walk_to_pmd(struct mm_struct *mm,
				       unsigned long address, int node)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset_node(mm, address, node);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return (pmd_t *)HYDRA_WALK_PGD_NONE;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return (pmd_t *)HYDRA_WALK_P4D_NONE;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return (pmd_t *)HYDRA_WALK_PUD_NONE;

	return pmd_offset(pud, address);
}

static inline pte_t *hydra_walk_to_pte(struct mm_struct *mm,
				       unsigned long address, int node)
{
	pmd_t *pmd;

	pmd = hydra_walk_to_pmd(mm, address, node);
	if (HYDRA_WALK_BAD(pmd))
		return (pte_t *)pmd;

	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)) ||
	    unlikely(pmd_trans_huge(*pmd)))
		return (pte_t *)HYDRA_WALK_PMD_NONE;

	return pte_offset_kernel(pmd, address);
}

struct hydra_cache_head {
	spinlock_t lock;
	struct page *head;
	atomic_t count;
	atomic64_t hits;
	atomic64_t misses;
	atomic64_t returns;
} ____cacheline_aligned_in_smp;

extern struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT];

extern bool hydra_cache_push(struct page *page, int node);
extern struct page *hydra_cache_pop(int node);
extern int hydra_cache_drain_node(int node);
extern int hydra_cache_drain_all(void);

static inline int hydra_collect_repl_nodes(struct page *const ptpage,
					   nodemask_t *nodemask)
{
	struct page *cur_page;
	struct page *start_page;

	start_page = ptpage;
	cur_page = ptpage;

	do {
		node_set(page_to_nid(cur_page), *nodemask);
		cur_page = cur_page->next_replica;
	} while (cur_page && cur_page != start_page);

	return 0;
}

static inline int hydra_calculate_tlbflush_nodemask(struct page *const ptpage,
						    nodemask_t *nodemask)
{
	switch (sysctl_hydra_tlbflush_opt) {
	case 1:
	case 2:
		return hydra_collect_repl_nodes(ptpage, nodemask);
	case 3:
		if (ptpage->next_replica && ptpage->next_replica != ptpage) {
			nodes_clear(*nodemask);
			nodes_or(*nodemask, *nodemask, node_online_map);
		}
		return 0;
	}
	return 1;
}

static inline void hydra_chain_lock(struct page *master)
{
	bit_spin_lock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

static inline void hydra_chain_unlock(struct page *master)
{
	bit_spin_unlock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

static inline void hydra_link_page_to_replica_chain(struct page *existing_page,
						    struct page *new_page)
{
	struct page *cur_page;
	struct page *start_page;
	struct page *next_repl;
	int chain_len = 0;

	if (!existing_page || !new_page || existing_page == new_page)
		return;

	if (WARN_ON(page_to_nid(new_page) == page_to_nid(existing_page)))
		return;

	hydra_chain_lock(existing_page);

	start_page = existing_page;
	cur_page = READ_ONCE(existing_page->next_replica);

	while (cur_page && cur_page != start_page) {
		chain_len++;
		if (cur_page == new_page)
			goto out_unlock;
		if (page_to_nid(cur_page) == page_to_nid(new_page)) {
		    pr_info("HYDRA: same-NID race: existing=%px(nid=%d) new=%px(nid=%d) "
			    "master=%px(nid=%d) chain_len=%d cpu=%d\n",
			    cur_page, page_to_nid(cur_page),
			    new_page, page_to_nid(new_page),
			    existing_page, page_to_nid(existing_page),
			    chain_len, smp_processor_id());
		    goto out_unlock;
		}
		if (WARN_ON(chain_len >= NUMA_NODE_COUNT))
			goto out_unlock;
		cur_page = READ_ONCE(cur_page->next_replica);
	}

	next_repl = existing_page->next_replica;
	new_page->next_replica = next_repl ? next_repl : existing_page;
	smp_wmb();
	existing_page->next_replica = new_page;

out_unlock:
	hydra_chain_unlock(existing_page);
}

static inline void hydra_break_chain(struct page *page)
{
	struct page *cur_page, *next_page;
	struct page *start_page;

	if (!page || !page->next_replica)
		return;

	start_page = page;

	hydra_chain_lock(page);
	cur_page = page->next_replica;
	page->next_replica = NULL;
	hydra_chain_unlock(page);

	while (cur_page && cur_page != start_page) {
		next_page = READ_ONCE(cur_page->next_replica);
		WRITE_ONCE(cur_page->next_replica, NULL);
		cur_page = next_page;
	}
}

static inline void hydra_unlink_single(struct page *anchor,
				       struct page *target)
{
	struct page *cur;

	if (!anchor || !target || anchor == target)
		return;

	hydra_chain_lock(anchor);

	if (!anchor->next_replica)
		goto out;

	if (anchor->next_replica == target) {
		anchor->next_replica = target->next_replica;
		target->next_replica = NULL;
		if (anchor->next_replica == anchor)
			anchor->next_replica = NULL;
		goto out;
	}

	cur = anchor->next_replica;
	while (cur && cur != anchor) {
		if (cur->next_replica == target) {
			cur->next_replica = target->next_replica;
			target->next_replica = NULL;
			break;
		}
		cur = cur->next_replica;
	}

out:
	hydra_chain_unlock(anchor);
}

extern void hydra_free_replica_chain(struct page *primary, int level);

static inline bool hydra_try_return_page(struct page *page)
{
	int nid = page_to_nid(page);
	bool from_cache = PageHydraFromCache(page);

	ClearPageHydraFromCache(page);
	page->next_replica = NULL;

	if (from_cache && hydra_cache_push(page, nid))
		return true;

	return false;
}

static inline void hydra_dtor_free_page(struct page *page)
{
	struct ptdesc *ptdesc = page_ptdesc(page);

	pagetable_dtor(ptdesc);

	if (hydra_try_return_page(page))
		return;

	pagetable_free(ptdesc);
}

static inline struct page *hydra_alloc_pt_page_near(struct mm_struct *mm,
						    gfp_t gfp,
						    void *parent)
{
	int node;
	struct page *page;

	if (parent && virt_addr_valid(parent))
		node = page_to_nid(virt_to_page(parent));
	else
		node = numa_node_id();

	page = hydra_cache_pop(node);
	if (!page)
		page = alloc_pages_node(node, gfp | __GFP_THISNODE, 0);
	if (page)
		page->pt_owner_mm = mm;

	return page;
}

static inline struct page *hydra_alloc_pt_page(struct mm_struct *mm,
					       gfp_t gfp, unsigned int order)
{
	struct page *page;

	page = alloc_pages(gfp, order);

	if (page)
		page->pt_owner_mm = mm;

	return page;
}

#endif
