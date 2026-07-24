#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>
#include <linux/nodemask.h>
#include <linux/highmem.h>
#include <linux/sched/mm.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

int sysctl_hydra_share_dist __read_mostly = -1;
int sysctl_hydra_rent_base __read_mostly = 8192;
int sysctl_hydra_prebuild __read_mostly = 1;
int sysctl_hydra_push __read_mostly = 1;
int sysctl_hydra_promote __read_mostly = 1;
int sysctl_hydra_promote_ms __read_mostly = 1000;

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

#define HYDRA_PROMOTE_MM_CAP 64
#define HYDRA_PROMOTE_LEAF_BATCH 16384

static bool hydra_promote_leaf(struct mm_struct *mm, unsigned long addr,
			       pmd_t *master_pmd, int node)
{
	struct page *master_page, *target;
	pgtable_t fresh;
	pmd_t *pmd_n;
	pte_t *mbase, *tbase;
	spinlock_t *pml, *ptl, *npml;
	nodemask_t flush;
	bool installed = false;
	int i;

	addr &= PMD_MASK;

	pmd_n = hydra_walk_to_pmd(mm, addr, node);
	if (HYDRA_WALK_BAD(pmd_n))
		return false;

	fresh = pte_alloc_one(mm, pmd_n);
	if (fresh && page_to_nid(fresh) != node) {
		hydra_free_chain_node_rcu(fresh);
		fresh = NULL;
	}

	pml = pmd_lock(mm, master_pmd);
	if (pmd_none(*master_pmd) || pmd_trans_huge(*master_pmd) ||
	    pmd_bad(*master_pmd))
		goto out_unlock;
	master_page = pmd_pgtable(*master_pmd);

	pmd_n = hydra_walk_to_pmd(mm, addr, node);
	if (HYDRA_WALK_BAD(pmd_n))
		goto out_unlock;

	{
		pmd_t nval = READ_ONCE(*pmd_n);

		if (!(pmd_flags(nval) & _PAGE_PRESENT) ||
		    pmd_trans_huge(nval) || pmd_leaf(nval) || pmd_bad(nval))
			goto out_unlock;
		if (page_to_nid(pmd_pgtable(nval)) == node)
			goto out_unlock;
	}

	target = hydra_find_parked(master_page, node);
	if (target) {
		hydra_unpark_prepare(target);
		if (mm->hydra_stats)
			atomic_long_inc(&mm->hydra_stats->coh_unparks);
	} else {
		if (!fresh)
			goto out_unlock;
		target = fresh;
		fresh = NULL;
		hydra_link_page_to_replica_chain(master_page, target);
		mm_inc_nr_ptes(mm);
		paravirt_alloc_pte(mm, page_to_pfn(target));
	}

	ptl = pte_lockptr(mm, master_pmd);
	if (ptl != pml)
		spin_lock_nested(ptl, SINGLE_DEPTH_NESTING);

	mbase = (pte_t *)page_address(master_page);
	tbase = (pte_t *)page_address(target);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte_t val = READ_ONCE(mbase[i]);
		pte_t cur = READ_ONCE(tbase[i]);

		if (!(pte_val(val) & _PAGE_PRESENT) || pte_protnone(val))
			continue;
		if (pte_val(cur) & _PAGE_PRESENT) {
			if ((pte_val(cur) ^ pte_val(val)) &
			    ~(_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SAVED_DIRTY)) {
				pr_emerg("HYDRA: promote target diverged under master PTL: idx %d master %lx target %lx m_pg %px (nid %d) t_pg %px (nid %d)\n",
					 i, pte_val(val), pte_val(cur),
					 master_page, page_to_nid(master_page),
					 target, page_to_nid(target));
				BUG();
			}
			continue;
		}
		native_set_pte(&tbase[i], val);
	}

	if (ptl != pml)
		spin_unlock(ptl);

	npml = pmd_lockptr(mm, pmd_n);
	if (npml != pml)
		spin_lock_nested(npml, SINGLE_DEPTH_NESTING);
	{
		pmd_t nval = READ_ONCE(*pmd_n);

		if ((pmd_flags(nval) & _PAGE_PRESENT) &&
		    !pmd_trans_huge(nval) && !pmd_leaf(nval) &&
		    !pmd_bad(nval) &&
		    page_to_nid(pmd_pgtable(nval)) != node) {
			native_set_pmd(pmd_n,
				__pmd(((pmdval_t)page_to_pfn(target) << PAGE_SHIFT)
				      | _PAGE_TABLE));
			hydra_map_node_clear(master_page, node);
			hydra_heat_clear(master_page, node);
			installed = true;
		}
	}
	if (npml != pml)
		spin_unlock(npml);

out_unlock:
	spin_unlock(pml);

	if (fresh)
		hydra_free_chain_node_rcu(fresh);

	if (installed) {
		nodes_clear(flush);
		node_set(node, flush);
		flush_tlb_mm_node_range(mm, addr, addr + PMD_SIZE, PAGE_SHIFT,
					true, &flush);
		if (mm->hydra_stats)
			atomic_long_inc(&mm->hydra_stats->coh_promotions);
	}

	return installed;
}

static void hydra_promote_check_leaf(struct mm_struct *mm, unsigned long addr,
				     pmd_t *master_pmd,
				     struct page *master_page)
{
	unsigned long act;
	int j;

	act = READ_ONCE(mm->hydra_active_nodes) & HYDRA_MAP_NODES_MASK;
	for_each_set_bit(j, &act, NUMA_NODE_COUNT) {
		pmd_t *pmd_j;
		pmd_t v;

		pmd_j = hydra_walk_to_pmd(mm, addr, j);
		if (HYDRA_WALK_BAD(pmd_j) || pmd_j == master_pmd)
			continue;
		v = READ_ONCE(*pmd_j);
		if (!hydra_pmd_entry_is_table(v) || pmd_bad(v))
			continue;
		if (page_to_nid(pmd_pgtable(v)) == j)
			continue;
		if (!pmd_young(v)) {
			if (hydra_heat_test(master_page, j))
				hydra_heat_clear(master_page, j);
			continue;
		}
		if (!hydra_heat_test(master_page, j)) {
			hydra_heat_set(master_page, j);
			clear_bit(_PAGE_BIT_ACCESSED, (unsigned long *)pmd_j);
			continue;
		}
		hydra_promote_leaf(mm, addr, master_pmd, j);
	}
}

static void hydra_promote_scan_mm(struct mm_struct *mm)
{
	unsigned long addr, end, next_pud;
	long scanned = 0;

	if (!mmap_read_trylock(mm))
		return;

	if (!mm->lazy_repl_enabled)
		goto out;

	end = mm->task_size;
	addr = mm->hydra_stats ? mm->hydra_stats->coh_scan_cursor : 0;
	if (addr >= end)
		addr = 0;

	while (addr < end && scanned < HYDRA_PROMOTE_LEAF_BATCH) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;

		next_pud = (addr & PUD_MASK) + PUD_SIZE;
		if (!next_pud || next_pud > end)
			next_pud = end;

		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd)) {
			addr = next_pud;
			continue;
		}
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			addr = next_pud;
			continue;
		}
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_trans_huge(*pud) || pud_bad(*pud)) {
			addr = next_pud;
			continue;
		}

		pmd = pmd_offset(pud, addr);
		while (addr < next_pud && scanned < HYDRA_PROMOTE_LEAF_BATCH) {
			pmd_t pv = READ_ONCE(*pmd);

			if (hydra_pmd_entry_is_table(pv) && !pmd_bad(pv)) {
				scanned++;
				hydra_promote_check_leaf(mm, addr & PMD_MASK,
							 pmd, pmd_pgtable(pv));
			}
			addr = (addr & PMD_MASK) + PMD_SIZE;
			pmd++;
		}
	}

	if (mm->hydra_stats)
		mm->hydra_stats->coh_scan_cursor = addr >= end ? 0 : addr;

out:
	mmap_read_unlock(mm);
}

static struct mm_struct *hydra_promote_mms[HYDRA_PROMOTE_MM_CAP];

static void hydra_promote_work_fn(struct work_struct *w);

static DECLARE_DELAYED_WORK(hydra_promote_work, hydra_promote_work_fn);

static void hydra_promote_work_fn(struct work_struct *w)
{
	int n, i;

	if (READ_ONCE(sysctl_hydra_promote) &&
	    static_branch_unlikely(&hydra_repl_ever_enabled) &&
	    hydra_topo.ready) {
		n = hydra_stats_collect_repl_mms(hydra_promote_mms,
						 HYDRA_PROMOTE_MM_CAP);
		for (i = 0; i < n; i++) {
			hydra_promote_scan_mm(hydra_promote_mms[i]);
			mmput(hydra_promote_mms[i]);
		}
	}

	schedule_delayed_work(&hydra_promote_work,
			      msecs_to_jiffies(READ_ONCE(sysctl_hydra_promote_ms)));
}

static int __init hydra_promote_init(void)
{
	schedule_delayed_work(&hydra_promote_work,
			      msecs_to_jiffies(READ_ONCE(sysctl_hydra_promote_ms)));
	return 0;
}
late_initcall(hydra_promote_init);
