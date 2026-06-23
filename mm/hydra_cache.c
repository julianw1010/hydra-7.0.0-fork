#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/hydra_util.h>

struct hydra_cache_head hydra_cache[NUMA_NODE_COUNT] = {
	[0 ... NUMA_NODE_COUNT - 1] = {
		.lock		= __SPIN_LOCK_UNLOCKED(hydra_cache.lock),
		.head		= NULL,
		.count		= ATOMIC_INIT(0),
		.hits		= ATOMIC64_INIT(0),
		.misses		= ATOMIC64_INIT(0),
		.returns	= ATOMIC64_INIT(0),
	}
};

bool hydra_cache_push(struct page *page, int node)
{
	struct hydra_cache_head *cache;
	unsigned long flags;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return false;

	cache = &hydra_cache[node];

	ClearPageHydraFromCache(page);

	spin_lock_irqsave(&cache->lock, flags);
	page->next_replica = cache->head;
	cache->head = page;
	spin_unlock_irqrestore(&cache->lock, flags);

	atomic_inc(&cache->count);
	atomic64_inc(&cache->returns);

	return true;
}

struct page *hydra_cache_pop(int node)
{
	struct hydra_cache_head *cache;
	struct page *page;
	unsigned long flags;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return NULL;

	cache = &hydra_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	if (!page) {
		spin_unlock_irqrestore(&cache->lock, flags);
		atomic64_inc(&cache->misses);
		return NULL;
	}
	cache->head = page->next_replica;
	spin_unlock_irqrestore(&cache->lock, flags);

	atomic_dec(&cache->count);
	atomic64_inc(&cache->hits);

	page->next_replica = NULL;
	SetPageHydraFromCache(page);

	clear_highpage(page);

	return page;
}

int hydra_cache_drain_node(int node)
{
	struct hydra_cache_head *cache;
	struct page *page, *next;
	unsigned long flags;
	int freed = 0;

	if (node < 0 || node >= NUMA_NODE_COUNT)
		return 0;

	cache = &hydra_cache[node];

	spin_lock_irqsave(&cache->lock, flags);
	page = cache->head;
	cache->head = NULL;
	spin_unlock_irqrestore(&cache->lock, flags);

	if (!page)
		return 0;

	atomic_set(&cache->count, 0);

	while (page) {
		next = page->next_replica;
		page->next_replica = NULL;
		ClearPageHydraFromCache(page);
		__free_page(page);
		freed++;
		page = next;
	}

	return freed;
}

int hydra_cache_drain_all(void)
{
	int node, total = 0;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		total += hydra_cache_drain_node(node);
	}

	return total;
}

/*
 * This function is ONLY called from the three pmdp_collapse_flush sites in
 * mm/khugepaged.c. THP collapse is the only operation that creates orphan
 * replica pages — pages unreachable from any top-down PGD walk but still
 * linked in the master PTE page's replica chain. pmdp_collapse_flush clears
 * all PMD entries (master + replicas) atomically, leaving replica PTE pages
 * findable only through the chain. We free them here immediately, so orphans
 * never persist. All other teardown (munmap, exit) uses hydra_break_chain_range
 * followed by per-PGD top-down walks, which works because no orphans exist.
 *
 * If you add a new pmdp_collapse_flush call site, you must call this function
 * on the old PTE page before freeing it, or replica pages will leak.
 */
void hydra_free_replica_chain(struct page *primary, int level)
{
	struct page *cur_page, *next_page;
	struct page *start_page;

	if (!primary || !primary->next_replica)
		return;

	start_page = primary;

	hydra_chain_lock(primary);
	cur_page = primary->next_replica;
	primary->next_replica = NULL;
	hydra_chain_unlock(primary);

	while (cur_page && cur_page != start_page) {
		struct mm_struct *owner_mm = cur_page->pt_owner_mm;

		next_page = READ_ONCE(cur_page->next_replica);
		cur_page->next_replica = NULL;

		if (level <= HYDRA_LEVEL_PMD)
			pagetable_dtor(page_ptdesc(cur_page));

		if (owner_mm) {
			if (level == HYDRA_LEVEL_PTE)
				mm_dec_nr_ptes(owner_mm);
			else if (level == HYDRA_LEVEL_PMD)
				mm_dec_nr_pmds(owner_mm);
		}

		if (!hydra_try_return_page(cur_page))
			__free_page(cur_page);

		cur_page = next_page;
	}
}
