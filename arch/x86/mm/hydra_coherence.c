#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>
#include <linux/nodemask.h>
#include <linux/highmem.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

int sysctl_hydra_share_dist __read_mostly = -1;
int sysctl_hydra_rent_base __read_mostly = 8192;
int sysctl_hydra_prebuild __read_mostly = 1;
int sysctl_hydra_push __read_mostly = 1;

struct page *hydra_pick_share_source(struct page *master_page, int node)
{
	struct page *cur = master_page;
	struct page *pick = NULL;
	int best = INT_MAX;
	int thr = hydra_share_dist_eff();

	if (thr <= 0)
		return NULL;

	do {
		if (!hydra_pt_parked(cur)) {
			int d = hydra_node_dist(node, page_to_nid(cur));

			if (d <= thr && d < best) {
				best = d;
				pick = cur;
			}
		}
		cur = READ_ONCE(cur->next_replica);
	} while (cur && cur != master_page);

	return pick;
}

struct page *hydra_find_parked(struct page *master_page, int node)
{
	struct page *cur;

	for (cur = READ_ONCE(master_page->next_replica);
	     cur && cur != master_page;
	     cur = READ_ONCE(cur->next_replica)) {
		if (page_to_nid(cur) == node && hydra_pt_parked(cur))
			return cur;
	}

	return NULL;
}

void hydra_unpark_prepare(struct page *page)
{
	memset(page_address(page), 0, PAGE_SIZE);
	hydra_rent_clear(page);
	hydra_pt_clear_parked(page);
}

pmd_t hydra_push_val(pmd_t master_e, int node, struct mm_struct *mm)
{
	struct page *child;
	struct page *src;

	if (!mm || !READ_ONCE(sysctl_hydra_push))
		return __pmd(0);
	if (!(READ_ONCE(mm->hydra_active_nodes) & (1UL << node)))
		return __pmd(0);

	child = pmd_pgtable(master_e);
	src = hydra_pick_share_source(child, node);
	if (!src)
		return __pmd(0);

	hydra_map_node_set(child, node);
	if (mm->hydra_stats)
		atomic_long_inc(&mm->hydra_stats->coh_push_installs);

	return __pmd(((pmdval_t)page_to_pfn(src) << PAGE_SHIFT) |
		     (pmd_val(master_e) & ~PTE_PFN_MASK));
}

void hydra_push_siblings(struct mm_struct *mm, unsigned long addr,
			 struct page *master_pte_page, struct page *target,
			 int self, spinlock_t *held_pml)
{
	unsigned long act;
	int k;

	if (!READ_ONCE(sysctl_hydra_push))
		return;

	act = READ_ONCE(mm->hydra_active_nodes) & HYDRA_MAP_NODES_MASK;
	for_each_set_bit(k, &act, NUMA_NODE_COUNT) {
		pmd_t *pmd_k;
		spinlock_t *kpml;

		if (k == self)
			continue;
		if (hydra_node_dist(k, page_to_nid(target)) >
		    hydra_share_dist_eff())
			continue;
		pmd_k = hydra_walk_to_pmd(mm, addr, k);
		if (HYDRA_WALK_BAD(pmd_k) || !pmd_none(*pmd_k))
			continue;

		kpml = pmd_lockptr(mm, pmd_k);
		if (kpml != held_pml)
			spin_lock_nested(kpml, SINGLE_DEPTH_NESTING);
		if (pmd_none(*pmd_k)) {
			hydra_map_node_set(master_pte_page, k);
			native_set_pmd(pmd_k,
				__pmd(((pmdval_t)page_to_pfn(target) << PAGE_SHIFT)
				      | _PAGE_TABLE));
			if (mm->hydra_stats)
				atomic_long_inc(&mm->hydra_stats->coh_shared_links);
		}
		if (kpml != held_pml)
			spin_unlock(kpml);
	}
}

static int hydra_rent_limit(int copy_nid, int master_nid)
{
	int base = READ_ONCE(sysctl_hydra_rent_base);
	int d = hydra_node_dist(copy_nid, master_nid);
	int floor = hydra_topo.min_offnode_dist ? hydra_topo.min_offnode_dist : 1;

	return base * d / floor;
}

static bool hydra_over_rent(struct page *master_page)
{
	struct page *cur;
	bool over = false;
	int mnid = page_to_nid(master_page);

	rcu_read_lock();
	for (cur = READ_ONCE(master_page->next_replica);
	     cur && cur != master_page;
	     cur = READ_ONCE(cur->next_replica)) {
		if (hydra_pt_parked(cur))
			continue;
		if (atomic_read(&cur->hydra_rent) >
		    hydra_rent_limit(page_to_nid(cur), mnid)) {
			over = true;
			break;
		}
	}
	rcu_read_unlock();

	return over;
}

static void hydra_clear_pte_refs(struct mm_struct *mm, unsigned long addr,
				 struct page *victim, struct page *master_page,
				 spinlock_t *held_pml, nodemask_t *flush)
{
	int j;

	for (j = 0; j < NUMA_NODE_COUNT; j++) {
		pmd_t *pmd_j;
		spinlock_t *jpml;

		pmd_j = hydra_walk_to_pmd(mm, addr, j);
		if (HYDRA_WALK_BAD(pmd_j))
			continue;
		if (pmd_none(*pmd_j) || pmd_trans_huge(*pmd_j) ||
		    pmd_bad(*pmd_j))
			continue;
		if (pmd_pgtable(*pmd_j) != victim)
			continue;

		jpml = pmd_lockptr(mm, pmd_j);
		if (jpml != held_pml)
			spin_lock_nested(jpml, SINGLE_DEPTH_NESTING);
		if (!pmd_none(*pmd_j) && !pmd_trans_huge(*pmd_j) &&
		    pmd_pgtable(*pmd_j) == victim)
			native_set_pmd(pmd_j, __pmd(0));
		if (jpml != held_pml)
			spin_unlock(jpml);

		node_set(j, *flush);
		if (master_page)
			hydra_map_node_clear(master_page, j);
		if (j != page_to_nid(victim) && mm->hydra_stats)
			atomic_long_inc(&mm->hydra_stats->coh_shared_ref_clears);
	}
}

static void hydra_park_pte_leaf(struct mm_struct *mm, pmd_t *master_pmd,
				unsigned long addr)
{
	struct page *mpage, *cur, *victims[NUMA_NODE_COUNT];
	int nvict = 0, i;
	nodemask_t flush;
	spinlock_t *pml;

	addr &= PMD_MASK;
	pml = pmd_lock(mm, master_pmd);
	if (pmd_none(*master_pmd) || pmd_trans_huge(*master_pmd) ||
	    pmd_bad(*master_pmd)) {
		spin_unlock(pml);
		return;
	}
	mpage = pmd_pgtable(*master_pmd);
	nodes_clear(flush);

	hydra_chain_lock(mpage);
	for (cur = mpage->next_replica; cur && cur != mpage;
	     cur = cur->next_replica) {
		if (hydra_pt_parked(cur))
			continue;
		if (atomic_read(&cur->hydra_rent) <=
		    hydra_rent_limit(page_to_nid(cur), page_to_nid(mpage)))
			continue;
		if (nvict < NUMA_NODE_COUNT)
			victims[nvict++] = cur;
	}

	if (!nvict) {
		hydra_chain_unlock(mpage);
		spin_unlock(pml);
		return;
	}

	for (i = 0; i < nvict; i++)
		hydra_clear_pte_refs(mm, addr, victims[i], mpage, pml, &flush);

	flush_tlb_mm_node_range(mm, addr, addr + PMD_SIZE, PAGE_SHIFT, true,
				&flush);

	for (i = 0; i < nvict; i++) {
		hydra_rent_clear(victims[i]);
		hydra_pt_set_parked(victims[i]);
	}
	if (mm->hydra_stats)
		atomic_long_add(nvict, &mm->hydra_stats->coh_pte_parks);

	hydra_chain_unlock(mpage);
	spin_unlock(pml);
}

static bool hydra_pmd_entry_is_table(pmd_t e)
{
	if (!(pmd_val(e) & _PAGE_PRESENT))
		return false;
	if (pmd_trans_huge(e) || pmd_leaf(e))
		return false;
	return true;
}

static void hydra_park_pmd_leaf(struct mm_struct *mm, pud_t *master_pud,
				unsigned long addr)
{
	struct page *mpage, *cur, *victims[NUMA_NODE_COUNT];
	pmd_t *master_pmd_base;
	int nvict = 0, i, k;
	nodemask_t flush;
	spinlock_t *pud_ptl, *pml;

	addr &= PUD_MASK;
	pud_ptl = pud_lock(mm, master_pud);
	if (pud_none(*master_pud) || pud_trans_huge(*master_pud) ||
	    pud_bad(*master_pud)) {
		spin_unlock(pud_ptl);
		return;
	}

	master_pmd_base = pmd_offset(master_pud, addr);
	mpage = virt_to_page(master_pmd_base);
	pml = pmd_lockptr(mm, master_pmd_base);
	spin_lock(pml);
	nodes_clear(flush);

	hydra_chain_lock(mpage);
	for (cur = mpage->next_replica; cur && cur != mpage;
	     cur = cur->next_replica) {
		if (hydra_pt_parked(cur))
			continue;
		if (atomic_read(&cur->hydra_rent) <=
		    hydra_rent_limit(page_to_nid(cur), page_to_nid(mpage)))
			continue;
		if (nvict < NUMA_NODE_COUNT)
			victims[nvict++] = cur;
	}

	if (!nvict) {
		hydra_chain_unlock(mpage);
		spin_unlock(pml);
		spin_unlock(pud_ptl);
		return;
	}

	for (i = 0; i < nvict; i++) {
		struct page *v = victims[i];
		int j = page_to_nid(v);
		pmd_t *vbase = page_address(v);
		pgd_t *pgd_j;
		p4d_t *p4d_j;
		pud_t *pud_j;

		pgd_j = pgd_offset_pgd(mm->repl_pgd[j], addr);
		if (!pgd_none(*pgd_j) && !pgd_bad(*pgd_j)) {
			p4d_j = p4d_offset(pgd_j, addr);
			if (!p4d_none(*p4d_j) && !p4d_bad(*p4d_j)) {
				pud_j = pud_offset(p4d_j, addr);
				if (!pud_none(*pud_j) &&
				    !pud_trans_huge(*pud_j) &&
				    pud_pgtable(*pud_j) == vbase) {
					native_set_pud(pud_j, __pud(0));
					node_set(j, flush);
				}
			}
		}

		for (k = 0; k < PTRS_PER_PMD; k++) {
			pmd_t e = READ_ONCE(vbase[k]);
			struct page *c;
			unsigned long caddr;
			struct page *mc = NULL;
			pmd_t me;

			if (!hydra_pmd_entry_is_table(e))
				continue;
			c = pmd_pgtable(e);
			if (page_to_nid(c) != j || hydra_pt_parked(c))
				continue;

			caddr = addr + ((unsigned long)k << PMD_SHIFT);
			me = READ_ONCE(master_pmd_base[k]);
			if (hydra_pmd_entry_is_table(me))
				mc = pmd_pgtable(me);

			hydra_clear_pte_refs(mm, caddr, c, mc, pml, &flush);
		}
	}

	flush_tlb_mm_node_range(mm, addr, addr + PUD_SIZE, PAGE_SHIFT, true,
				&flush);

	for (i = 0; i < nvict; i++) {
		struct page *v = victims[i];
		pmd_t *vbase = page_address(v);

		for (k = 0; k < PTRS_PER_PMD; k++) {
			pmd_t e = READ_ONCE(vbase[k]);
			struct page *c;

			if (!hydra_pmd_entry_is_table(e))
				continue;
			c = pmd_pgtable(e);
			if (page_to_nid(c) != page_to_nid(v) ||
			    hydra_pt_parked(c))
				continue;
			hydra_rent_clear(c);
			hydra_pt_set_parked(c);
			if (mm->hydra_stats)
				atomic_long_inc(&mm->hydra_stats->coh_pte_parks);
		}

		hydra_rent_clear(v);
		hydra_pt_set_parked(v);
	}
	if (mm->hydra_stats)
		atomic_long_add(nvict, &mm->hydra_stats->coh_pmd_parks);

	hydra_chain_unlock(mpage);
	spin_unlock(pml);
	spin_unlock(pud_ptl);
}

void hydra_coherence_enforce(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pmd_t pmdval;

	if (!mm->lazy_repl_enabled || !READ_ONCE(sysctl_hydra_rent_base) ||
	    !hydra_topo.ready)
		return;

	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return;
	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return;
	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || pud_trans_huge(*pud) || pud_bad(*pud))
		return;
	pmd = pmd_offset(pud, address);
	pmdval = READ_ONCE(*pmd);
	if (pmd_none(pmdval))
		return;

	if (pmd_trans_huge(pmdval)) {
		if (hydra_over_rent(virt_to_page(pmd)))
			hydra_park_pmd_leaf(mm, pud, address);
		return;
	}

	if (pmd_bad(pmdval))
		return;

	if (hydra_over_rent(pmd_pgtable(pmdval)))
		hydra_park_pte_leaf(mm, pmd, address);
}
