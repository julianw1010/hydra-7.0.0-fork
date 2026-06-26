#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/page.h>

static unsigned long hydra_get_entry(void *entryp)
{
	struct page *page, *cur;
	unsigned long offset, val;

	if (!entryp)
		return 0;

	if (!virt_addr_valid(entryp))
		return *(unsigned long *)entryp;

	page = virt_to_page(entryp);

	if (!READ_ONCE(page->next_replica))
		return *(unsigned long *)entryp;

	val = *(unsigned long *)entryp;
	if (!(val & _PAGE_PRESENT))
		return val;

	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		unsigned long entry_val = *(unsigned long *)(page_address(cur) + offset);
		if (entry_val & _PAGE_PRESENT)
			val |= entry_val & PTE_FLAGS_MASK;
	}
	rcu_read_unlock();

	return val;
}

static unsigned long hydra_get_and_clear_entry(void *entryp)
{
	struct page *page, *cur;
	unsigned long offset, val;

	if (!entryp)
		return 0;

	if (!virt_addr_valid(entryp))
		return xchg((unsigned long *)entryp, 0);

	page = virt_to_page(entryp);

	if (!READ_ONCE(page->next_replica))
		return xchg((unsigned long *)entryp, 0);

	val = xchg((unsigned long *)entryp, 0);
	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		unsigned long old_val = xchg((unsigned long *)(page_address(cur) + offset), 0);
		if (old_val & _PAGE_PRESENT)
			val |= old_val & PTE_FLAGS_MASK;
	}
	rcu_read_unlock();

	return val;
}

static void hydra_set_wrprotect_entry(void *entryp)
{
	struct page *page, *cur;
	unsigned long offset;

	if (!entryp)
		return;

	if (!virt_addr_valid(entryp)) {
		clear_bit(_PAGE_BIT_RW, (unsigned long *)entryp);
		return;
	}

	page = virt_to_page(entryp);

	if (!READ_ONCE(page->next_replica)) {
		clear_bit(_PAGE_BIT_RW, (unsigned long *)entryp);
		return;
	}

	clear_bit(_PAGE_BIT_RW, (unsigned long *)entryp);
	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur) + offset);
		if (*replica_entry & _PAGE_PRESENT)
			clear_bit(_PAGE_BIT_RW, replica_entry);
	}
	rcu_read_unlock();
}

static int hydra_test_and_clear_young_entry(void *entryp)
{
	struct page *page, *cur;
	unsigned long offset;
	int young = 0;

	if (!entryp)
		return 0;

	if (!virt_addr_valid(entryp)) {
		if (test_bit(_PAGE_BIT_ACCESSED, (unsigned long *)entryp))
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						   (unsigned long *)entryp);
		return young;
	}

	page = virt_to_page(entryp);

	if (!READ_ONCE(page->next_replica)) {
		if (test_bit(_PAGE_BIT_ACCESSED, (unsigned long *)entryp))
			young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
						   (unsigned long *)entryp);
		return young;
	}

	if (test_bit(_PAGE_BIT_ACCESSED, (unsigned long *)entryp))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)entryp);

	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur) + offset);
		if ((*replica_entry & _PAGE_PRESENT) &&
		    test_bit(_PAGE_BIT_ACCESSED, replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED, replica_entry))
				young = 1;
		}
	}
	rcu_read_unlock();

	return young;
}

void hydra_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *pte_page, *cur;
	unsigned long offset;
	pte_t repl_val;

	native_set_pte(ptep, pteval);

	if (!virt_addr_valid(ptep))
		return;

	pte_page = virt_to_page(ptep);

	if (!READ_ONCE(pte_page->next_replica))
		return;

	offset = ((unsigned long)ptep) & ~PAGE_MASK;
	repl_val = pte_present(pteval) ? pteval : __pte(0);

	rcu_read_lock();
	for (cur = pte_page->next_replica; cur && cur != pte_page; cur = cur->next_replica) {
		pte_t *rp = (pte_t *)(page_address(cur) + offset);
		native_set_pte(rp, repl_val);
	}
	rcu_read_unlock();
}

pte_t hydra_get_pte(pte_t *ptep)
{
	return __pte(hydra_get_entry(ptep));
}

pte_t hydra_ptep_get_and_clear(struct mm_struct *mm, pte_t *ptep)
{
	return __pte(hydra_get_and_clear_entry(ptep));
}

void hydra_ptep_set_wrprotect(struct mm_struct *mm,
			      unsigned long addr, pte_t *ptep)
{
	hydra_set_wrprotect_entry(ptep);
}

int hydra_ptep_test_and_clear_young(struct vm_area_struct *vma,
				    unsigned long addr, pte_t *ptep)
{
	return hydra_test_and_clear_young_entry(ptep);
}

void hydra_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	struct page *page, *cur;
	unsigned long offset;
	pmd_t repl_val;

	native_set_pmd(pmdp, pmd);

	if (!virt_addr_valid(pmdp))
		return;

	page = virt_to_page(pmdp);

	if (!READ_ONCE(page->next_replica))
		return;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (pmd_present(pmd) && (pmd_trans_huge(pmd) || pmd_leaf(pmd)))
		repl_val = pmd;
	else
		repl_val = __pmd(0);

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page; cur = cur->next_replica) {
		pmd_t *rp = (pmd_t *)(page_address(cur) + offset);
		native_set_pmd(rp, repl_val);
	}
	rcu_read_unlock();
}

pmd_t hydra_get_pmd(pmd_t *pmdp)
{
	return __pmd(hydra_get_entry(pmdp));
}

pmd_t hydra_pmdp_get_and_clear(struct mm_struct *mm, pmd_t *pmdp)
{
	return __pmd(hydra_get_and_clear_entry(pmdp));
}

void hydra_pmdp_set_wrprotect(struct mm_struct *mm,
			      unsigned long addr, pmd_t *pmdp)
{
	hydra_set_wrprotect_entry(pmdp);
}

int hydra_pmdp_test_and_clear_young(struct vm_area_struct *vma,
				    unsigned long addr, pmd_t *pmdp)
{
	return hydra_test_and_clear_young_entry(pmdp);
}

pmd_t hydra_pmdp_establish(pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page, *cur;
	unsigned long offset;
	pmd_t old, repl_val;
	pmdval_t flags = 0;

	if (IS_ENABLED(CONFIG_SMP))
		old = xchg(pmdp, pmd);
	else {
		old = *pmdp;
		WRITE_ONCE(*pmdp, pmd);
	}

	if (!virt_addr_valid(pmdp))
		return old;

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica))
		return old;

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	if (pmd_present(pmd) && (pmd_trans_huge(pmd) || pmd_leaf(pmd)))
		repl_val = pmd;
	else
		repl_val = __pmd(0);

	rcu_read_lock();
	for (cur = pmd_page->next_replica; cur && cur != pmd_page; cur = cur->next_replica) {
		pmd_t *rp = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl;

		if (IS_ENABLED(CONFIG_SMP))
			old_repl = xchg(rp, repl_val);
		else {
			old_repl = *rp;
			WRITE_ONCE(*rp, repl_val);
		}

		if (pmd_trans_huge(old_repl) || pmd_leaf(old_repl))
			flags |= pmd_flags(old_repl);
	}
	rcu_read_unlock();

	return pmd_set_flags(old, flags);
}
