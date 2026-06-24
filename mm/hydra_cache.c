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

void hydra_free_replica_chain(struct page *primary, int level)
{
	struct page *cur_page, *next_page;
	struct page *start_page;

	if (!primary || !primary->next_replica)
		return;

	HYDRA_STAT_INC(free_replica_chain_calls);
	if (level == HYDRA_LEVEL_PTE)
		HYDRA_STAT_INC(free_replica_chain_pte_calls);
	else if (level == HYDRA_LEVEL_PMD)
		HYDRA_STAT_INC(free_replica_chain_pmd_calls);

	start_page = primary;

	hydra_chain_lock(primary);
	cur_page = primary->next_replica;
	primary->next_replica = NULL;
	hydra_chain_unlock(primary);

	while (cur_page && cur_page != start_page) {
		struct mm_struct *owner_mm = cur_page->pt_owner_mm;
		unsigned long *entry;
		int i, has_live_entry = 0;

		next_page = READ_ONCE(cur_page->next_replica);
		cur_page->next_replica = NULL;

		if (level == HYDRA_LEVEL_PTE) {
			entry = (unsigned long *)page_address(cur_page);
			for (i = 0; i < 512; i++) {
				if (entry[i] & _PAGE_PRESENT) {
					has_live_entry = 1;
					break;
				}
			}
			if (has_live_entry)
				HYDRA_STAT_INC(free_replica_chain_live_pte_bug);
			BUG_ON(has_live_entry);
		}

		if (level <= HYDRA_LEVEL_PMD)
			pagetable_dtor(page_ptdesc(cur_page));

		if (owner_mm) {
			if (level == HYDRA_LEVEL_PTE)
				mm_dec_nr_ptes(owner_mm);
			else if (level == HYDRA_LEVEL_PMD)
				mm_dec_nr_pmds(owner_mm);
		}

		HYDRA_STAT_INC(free_replica_chain_pages_freed);
		if (hydra_try_return_page(cur_page)) {
			HYDRA_STAT_INC(free_replica_chain_pages_to_cache);
		} else {
			HYDRA_STAT_INC(free_replica_chain_pages_to_buddy);
			__free_page(cur_page);
		}

		cur_page = next_page;
	}
}
