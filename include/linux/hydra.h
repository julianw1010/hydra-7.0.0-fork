#ifndef _LINUX_HYDRA_H
#define _LINUX_HYDRA_H

#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <linux/nodemask.h>
#include <linux/atomic.h>
#include <linux/highmem.h>
#include <linux/bitmap.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

void hydra_reload_cr3(void *info);
int hydra_enable_replication(struct mm_struct *mm);
int hydra_repl_fault(struct vm_fault *vmf, int fault_node);
void hydra_break_chain_range(struct mm_struct *mm,
			     unsigned long start, unsigned long end);
void hydra_map_ldt_to_replicas(struct mm_struct *mm);

#define HYDRA_WALK_NONE ((void *)0x1)

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
		return (pmd_t *)HYDRA_WALK_NONE;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return (pmd_t *)HYDRA_WALK_NONE;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return (pmd_t *)HYDRA_WALK_NONE;

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
		return (pte_t *)HYDRA_WALK_NONE;

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

	rcu_read_lock();
	do {
		node_set(page_to_nid(cur_page), *nodemask);
		cur_page = cur_page->next_replica;
	} while (cur_page && cur_page != start_page);
	rcu_read_unlock();

	return 0;
}

static inline void hydra_chain_lock(struct page *master)
{
	bit_spin_lock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

static inline void hydra_chain_unlock(struct page *master)
{
	bit_spin_unlock(PG_hydra_chain_locked, (unsigned long *)&master->flags);
}

void hydra_link_page_to_replica_chain(struct page *existing_page,
				      struct page *new_page);
void hydra_break_chain(struct page *page);
void hydra_unlink_single(struct page *anchor, struct page *target);

extern void hydra_free_replica_chain(struct page *primary);
void hydra_free_chain_node_rcu(struct page *page);

bool hydra_try_return_page(struct page *page);
void hydra_dtor_free_page(struct page *page);
struct page *hydra_alloc_pt_page_near(struct mm_struct *mm, gfp_t gfp,
				      void *parent);
struct page *hydra_alloc_pt_page(struct mm_struct *mm, gfp_t gfp,
				 unsigned int order);

#endif
