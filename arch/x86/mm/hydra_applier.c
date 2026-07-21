#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>

struct hydra_apply_work {
	struct kthread_work work;
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	int socket;
	long applied;
	int done;
};

#define HYDRA_SCOPE_SLOTS 4

struct hydra_scope_pool {
	struct hydra_apply_work slot[HYDRA_SCOPE_SLOTS];
	unsigned long tot_min[NUMA_NODE_COUNT];
	unsigned long tot_max[NUMA_NODE_COUNT];
	int used[HYDRA_SCOPE_SLOTS];
	long applied;
	long dispatched;
};

struct hydra_backfill_work {
	struct kthread_work work;
	struct mm_struct *mm;
	struct hydra_fill_info info;
	nodemask_t targets;
	size_t src_node;
	size_t master_node;
};

static struct kthread_worker *hydra_appliers[NUMA_NODE_COUNT];
static struct kthread_worker *hydra_backfillers[NUMA_NODE_COUNT];
int hydra_wrprot_delegation_ready;

static void hydra_backfill_fn(struct kthread_work *work)
{
	struct hydra_backfill_work *w =
		container_of(work, struct hydra_backfill_work, work);
	struct mm_struct *mm = w->mm;
	struct vm_area_struct *vma;
	int dsts[NUMA_NODE_COUNT];
	unsigned long start;
	int node, n = 0, i;

	if (!sysctl_hydra_extended || !READ_ONCE(mm->lazy_repl_enabled))
		goto out;

	start = w->info.level == HYDRA_PT_PMD ? w->info.address : w->info.start;

	mmap_read_lock(mm);

	if (!mm->lazy_repl_enabled)
		goto unlock;

	vma = find_vma(mm, start);
	if (!vma || vma->vm_start > start ||
	    vma->master_pgd_node != w->master_node)
		goto unlock;
	if (w->info.level != HYDRA_PT_PMD && w->info.end > vma->vm_end)
		goto unlock;

	for_each_node_mask(node, w->targets)
		dsts[n++] = node;
	if (!n)
		goto unlock;

	hydra_backfill_range(mm, vma, &w->info, dsts, n, w->src_node,
			     w->master_node);

	for (i = 0; i < n; i++)
		hydra_stats_fill_sweep(mm);

unlock:
	mmap_read_unlock(mm);
out:
	mmput_async(mm);
	kfree(w);
}

bool hydra_queue_backfill(struct mm_struct *mm, int socket, size_t src_node,
			  size_t master_node, const nodemask_t *targets,
			  const struct hydra_fill_info *info)
{
	struct hydra_backfill_work *w;

	if (!hydra_wrprot_delegation_ready)
		return false;

	w = kmalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return false;

	w->mm = mm;
	w->info = *info;
	w->targets = *targets;
	w->src_node = src_node;
	w->master_node = master_node;
	mmget(mm);
	kthread_init_work(&w->work, hydra_backfill_fn);
	kthread_queue_work(hydra_backfillers[socket], &w->work);
	return true;
}

static long hydra_reconcile_pmd_entry(struct mm_struct *mm, pmd_t *pmd,
				      int socket)
{
	spinlock_t *ptl;
	struct page *master_page, *cur;
	unsigned long offset;
	long applied = 0;
	pmd_t m;

	ptl = pmd_lock(mm, pmd);
	m = *pmd;

	master_page = virt_to_page(pmd);
	offset = (unsigned long)pmd & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = master_page->next_replica; cur && cur != master_page;
	     cur = cur->next_replica) {
		pmd_t *rp;
		pmd_t v, new;

		if (hydra_node_to_socket(page_to_nid(cur)) != socket)
			continue;
		rp = (pmd_t *)(page_address(cur) + offset);
		v = READ_ONCE(*rp);
		if (!(pmd_val(v) & _PAGE_PRESENT) || !pmd_trans_huge(v))
			continue;
		if (pmd_present(m) && pmd_trans_huge(m) && !pmd_protnone(m) &&
		    pmd_pfn(m) == pmd_pfn(v)) {
			unsigned long diff = (pmd_val(v) ^ pmd_val(m)) &
				~(_PAGE_ACCESSED | _PAGE_DIRTY |
				  _PAGE_SAVED_DIRTY);

			if (!diff)
				continue;
			if (diff == _PAGE_RW && pmd_write(v)) {
				hydra_wrprotect_pmd_one(rp);
				hydra_stats_rec_wrprot(mm);
				applied++;
				continue;
			}
			if (!pmd_write(v) && pmd_write(m) && !pmd_dirty(m))
				continue;
			new = m;
			if (pmd_young(v))
				new = pmd_mkyoung(new);
			if (pmd_dirty(v))
				new = pmd_mkdirty(new);
			if (pmd_write(new) && !pmd_dirty(m))
				new = pmd_wrprotect(new);
		} else if (pmd_present(m) && pmd_trans_huge(m) &&
			   !pmd_protnone(m)) {
			new = m;
			if (pmd_write(new) && !pmd_dirty(new))
				new = pmd_wrprotect(new);
		} else {
			new = __pmd(0);
		}
		if (try_cmpxchg((long *)rp, (long *)&v, *(long *)&new)) {
			if (pmd_val(new))
				hydra_stats_rec_synced(mm);
			else
				hydra_stats_rec_cleared(mm);
			applied++;
		}
	}
	rcu_read_unlock();

	spin_unlock(ptl);
	return applied;
}

static long hydra_reconcile_pte_range(struct mm_struct *mm, pmd_t *pmd,
				      unsigned long addr, unsigned long end,
				      int socket)
{
	spinlock_t *ptl;
	pte_t *pte, *base;
	struct page *master_page, *cur;
	unsigned long start_idx, end_idx, i;
	long applied = 0;

	pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!pte)
		return 0;

	base = (pte_t *)((unsigned long)pte & PAGE_MASK);
	master_page = virt_to_page(base);
	start_idx = (addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1);
	end_idx = start_idx + ((end - addr) >> PAGE_SHIFT);

	rcu_read_lock();
	for (cur = master_page->next_replica; cur && cur != master_page;
	     cur = cur->next_replica) {
		if (hydra_node_to_socket(page_to_nid(cur)) != socket)
			continue;
		for (i = start_idx; i < end_idx; i++) {
			pte_t m = base[i];
			pte_t *rp;
			pte_t v, new;

			rp = (pte_t *)(page_address(cur) + i * sizeof(pte_t));
			v = READ_ONCE(*rp);
			if (!(pte_val(v) & _PAGE_PRESENT))
				continue;
			if (pte_present(m) && !pte_protnone(m) &&
			    pte_pfn(m) == pte_pfn(v)) {
				unsigned long diff = (pte_val(v) ^ pte_val(m)) &
					~(_PAGE_ACCESSED | _PAGE_DIRTY |
					  _PAGE_SAVED_DIRTY);

				if (!diff)
					continue;
				if (diff == _PAGE_RW && pte_write(v)) {
					hydra_wrprotect_pte_one(rp);
					hydra_stats_rec_wrprot(mm);
					applied++;
					continue;
				}
				if (!pte_write(v) && pte_write(m) &&
				    !pte_dirty(m))
					continue;
				new = m;
				if (pte_young(v))
					new = pte_mkyoung(new);
				if (pte_dirty(v))
					new = pte_mkdirty(new);
				if (pte_write(new) && !pte_dirty(m))
					new = pte_wrprotect(new);
			} else if (pte_present(m) && !pte_protnone(m)) {
				new = m;
				if (pte_write(new) && !pte_dirty(new))
					new = pte_wrprotect(new);
			} else {
				new = __pte(0);
			}
			if (try_cmpxchg((long *)&rp->pte, (long *)&v,
					*(long *)&new)) {
				if (pte_val(new))
					hydra_stats_rec_synced(mm);
				else
					hydra_stats_rec_cleared(mm);
				applied++;
			}
		}
	}
	rcu_read_unlock();

	pte_unmap_unlock(pte, ptl);
	return applied;
}

static void hydra_reconcile_fn(struct kthread_work *work)
{
	struct hydra_apply_work *w =
		container_of(work, struct hydra_apply_work, work);
	struct mm_struct *mm = w->mm;
	unsigned long addr, next;

	for (addr = w->start; addr < w->end; addr = next) {
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;

		next = pmd_addr_end(addr, w->end);

		pgd = pgd_offset(mm, addr);
		if (pgd_none(*pgd) || pgd_bad(*pgd)) {
			next = pgd_addr_end(addr, w->end);
			continue;
		}
		p4d = p4d_offset(pgd, addr);
		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			next = p4d_addr_end(addr, w->end);
			continue;
		}
		pud = pud_offset(p4d, addr);
		if (pud_none(*pud) || pud_bad(*pud)) {
			next = pud_addr_end(addr, w->end);
			continue;
		}
		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd) || pmd_trans_huge(*pmd)) {
			w->applied += hydra_reconcile_pmd_entry(mm, pmd,
								w->socket);
			continue;
		}
		if (pmd_bad(*pmd))
			continue;
		w->applied += hydra_reconcile_pte_range(mm, pmd, addr, next,
							w->socket);
		cond_resched();
	}

	smp_store_release(&w->done, 1);
}

void hydra_scope_enter(struct hydra_scope *scope, struct mm_struct *mm)
{
	int s;

	scope->mm = mm;
	for (s = 0; s < NUMA_NODE_COUNT; s++) {
		scope->min[s] = ULONG_MAX;
		scope->max[s] = 0;
	}
	scope->pool = NULL;
	if (sysctl_hydra_extended && hydra_wrprot_delegation_ready &&
	    mm && mm->lazy_repl_enabled) {
		struct hydra_scope_pool *p =
			kzalloc(sizeof(*p), GFP_KERNEL | __GFP_NOWARN);

		if (p) {
			for (s = 0; s < NUMA_NODE_COUNT; s++) {
				p->tot_min[s] = ULONG_MAX;
				p->tot_max[s] = 0;
			}
			scope->pool = p;
		}
	}
	scope->prev = current->hydra_scope;
	current->hydra_scope = scope;
}

void hydra_scope_exit(struct hydra_scope *scope)
{
	struct hydra_scope_pool *p = scope->pool;

	if (p) {
		int i;

		for (i = 0; i < HYDRA_SCOPE_SLOTS; i++) {
			if (p->used[i])
				kthread_flush_work(&p->slot[i].work);
		}
		kfree(p);
		scope->pool = NULL;
	}
	current->hydra_scope = scope->prev;
}

void hydra_scope_dispatch(struct hydra_scope *scope, int socket)
{
	struct hydra_scope_pool *p = scope->pool;
	struct hydra_apply_work *w = NULL;
	int i, idx = -1;

	if (!hydra_wrprot_delegation_ready || !scope->mm ||
	    !scope->mm->lazy_repl_enabled)
		return;

	for (i = 0; i < HYDRA_SCOPE_SLOTS; i++) {
		if (!p->used[i]) {
			idx = i;
			break;
		}
		if (smp_load_acquire(&p->slot[i].done)) {
			p->applied += p->slot[i].applied;
			p->used[i] = 0;
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return;

	if (p->tot_min[socket] > scope->min[socket])
		p->tot_min[socket] = scope->min[socket];
	if (p->tot_max[socket] < scope->max[socket])
		p->tot_max[socket] = scope->max[socket];

	w = &p->slot[idx];
	w->mm = scope->mm;
	w->start = scope->min[socket];
	w->end = scope->max[socket];
	w->socket = socket;
	w->applied = 0;
	w->done = 0;
	kthread_init_work(&w->work, hydra_reconcile_fn);
	kthread_queue_work(hydra_appliers[socket], &w->work);
	p->used[idx] = 1;
	p->dispatched++;

	scope->min[socket] = ULONG_MAX;
	scope->max[socket] = 0;
}

bool hydra_scope_drain(struct hydra_scope *scope, unsigned long *lo_out,
		       unsigned long *hi_out)
{
	struct mm_struct *mm = scope->mm;
	struct hydra_scope_pool *p = scope->pool;
	struct hydra_apply_work works[NUMA_NODE_COUNT];
	unsigned long lo = ULONG_MAX, hi = 0;
	long applied = 0, items = 0;
	int s, n = 0, i;

	if (!hydra_wrprot_delegation_ready || !mm || !mm->lazy_repl_enabled)
		return false;

	for (s = 0; s < hydra_nr_sockets; s++) {
		unsigned long wlo = scope->min[s];
		unsigned long whi = scope->max[s];

		if (wlo >= whi)
			continue;

		scope->min[s] = ULONG_MAX;
		scope->max[s] = 0;

		if (lo > wlo)
			lo = wlo;
		if (hi < whi)
			hi = whi;

		works[n].mm = mm;
		works[n].start = wlo;
		works[n].end = whi;
		works[n].socket = s;
		works[n].applied = 0;
		works[n].done = 0;
		kthread_init_work(&works[n].work, hydra_reconcile_fn);
		kthread_queue_work(hydra_appliers[s], &works[n].work);
		n++;
	}

	if (p) {
		for (i = 0; i < HYDRA_SCOPE_SLOTS; i++) {
			if (!p->used[i])
				continue;
			kthread_flush_work(&p->slot[i].work);
			p->applied += p->slot[i].applied;
			p->used[i] = 0;
		}
		applied += p->applied;
		items = p->dispatched;
		p->applied = 0;
		p->dispatched = 0;
		for (s = 0; s < hydra_nr_sockets; s++) {
			if (p->tot_min[s] < p->tot_max[s]) {
				if (lo > p->tot_min[s])
					lo = p->tot_min[s];
				if (hi < p->tot_max[s])
					hi = p->tot_max[s];
			}
			p->tot_min[s] = ULONG_MAX;
			p->tot_max[s] = 0;
		}
	}

	if (!n && !items)
		return false;

	for (i = 0; i < n; i++) {
		kthread_flush_work(&works[i].work);
		applied += works[i].applied;
	}

	hydra_stats_sibling_reconciled(mm, applied);
	hydra_stats_scope_drain(mm, n + items);

	if (lo_out)
		*lo_out = lo;
	if (hi_out)
		*hi_out = hi;
	return true;
}

static int __init hydra_applier_init(void)
{
	int s;

	for (s = 0; s < hydra_nr_sockets; s++) {
		struct kthread_worker *worker;

		worker = kthread_create_worker(0, "hydra_apply/%d", s);
		if (IS_ERR(worker)) {
			pr_emerg("HYDRA: applier worker creation failed for socket %d\n",
				 s);
			BUG();
		}
		kthread_bind_mask(worker->task, &hydra_socket_cpumask[s]);
		wake_up_process(worker->task);
		hydra_appliers[s] = worker;

		worker = kthread_create_worker(0, "hydra_backfill/%d", s);
		if (IS_ERR(worker)) {
			pr_emerg("HYDRA: backfill worker creation failed for socket %d\n",
				 s);
			BUG();
		}
		kthread_bind_mask(worker->task, &hydra_socket_cpumask[s]);
		wake_up_process(worker->task);
		hydra_backfillers[s] = worker;
	}
	smp_wmb();
	hydra_wrprot_delegation_ready = 1;
	pr_info("HYDRA: appliers ready on %d socket(s)\n", hydra_nr_sockets);
	return 0;
}
late_initcall(hydra_applier_init);
