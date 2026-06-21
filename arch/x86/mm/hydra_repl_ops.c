#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/hydra_util.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/page.h>

void pgtable_track_set_pgd(pgd_t *pgdp, pgd_t pgd)
{
	native_set_pgd(pgdp, pgd);
}

void pgtable_track_set_p4d(p4d_t *p4dp, p4d_t p4d)
{
	native_set_p4d(p4dp, p4d);
}

void pgtable_track_set_pud(pud_t *pudp, pud_t pud)
{
	native_set_pud(pudp, pud);
}

void pgtable_track_set_pmd(pmd_t *pmdp, pmd_t pmd)
{
	struct page *page;
	struct page *cur;
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
		repl_val = pmd_mkold(pmd);
	else
		repl_val = __pmd(0);

	for_each_replica(page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		native_set_pmd(replica_entry, repl_val);
	}
}

void pgtable_repl_set_pte(pte_t *ptep, pte_t pteval)
{
	struct page *start_pte_page;
	struct page *cur;
	long offset;

	native_set_pte(ptep, pteval);

	if (!virt_addr_valid(ptep))
		return;

	start_pte_page = virt_to_page(ptep);

	if (!READ_ONCE(start_pte_page->next_replica)) {
		return;
	}

	offset = (long)ptep - (long)page_to_virt(start_pte_page);

	if (!pte_present(pteval)) {
		for_each_replica(start_pte_page, cur) {
			pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
			native_set_pte(rp, __pte(0));
		}
	} else {
		for_each_replica(start_pte_page, cur) {
			pte_t *rp = (pte_t *)((long)page_to_virt(cur) + offset);
			pte_t repl_val = pte_mkold(pteval);
			native_set_pte(rp, repl_val);
		}
	}

}

static unsigned long repl_get_entry(void *entryp)
{
	struct page *page;
	struct page *cur;
	unsigned long offset;
	unsigned long val;

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

	for_each_replica(page, cur) {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur) + offset);
		unsigned long entry_val = *replica_entry;

		if (entry_val & _PAGE_PRESENT)
			val |= entry_val & PTE_FLAGS_MASK;
	}

	return val;
}

static unsigned long repl_get_and_clear_entry(void *entryp)
{
	struct page *page;
	struct page *cur;
	unsigned long offset;
	unsigned long val;

	if (!entryp)
		return 0;

	if (!virt_addr_valid(entryp))
		return xchg((unsigned long *)entryp, 0);

	page = virt_to_page(entryp);

	if (!READ_ONCE(page->next_replica))
		return xchg((unsigned long *)entryp, 0);

	val = xchg((unsigned long *)entryp, 0);
	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	for_each_replica(page, cur) {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur) + offset);
		unsigned long old_val = xchg(replica_entry, 0);

		if (old_val & _PAGE_PRESENT)
			val |= old_val & PTE_FLAGS_MASK;
	}

	return val;
}

static void repl_set_wrprotect_entry(void *entryp)
{
	struct page *page;
	struct page *cur;
	unsigned long offset;

	if (!entryp)
		return;

	if (!virt_addr_valid(entryp)) {
		clear_bit(_PAGE_BIT_RW, (unsigned long *)entryp);
		return;
	}

	page = virt_to_page(entryp);

	clear_bit(_PAGE_BIT_RW, (unsigned long *)entryp);

	if (!READ_ONCE(page->next_replica))
		return;

	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	for_each_replica(page, cur) {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur) + offset);

		if (*replica_entry & _PAGE_PRESENT)
			clear_bit(_PAGE_BIT_RW, replica_entry);
	}
}

static int repl_test_and_clear_young_entry(void *entryp)
{
	struct page *page;
	struct page *cur;
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

	if (test_bit(_PAGE_BIT_ACCESSED, (unsigned long *)entryp))
		young = test_and_clear_bit(_PAGE_BIT_ACCESSED,
					   (unsigned long *)entryp);

	if (!READ_ONCE(page->next_replica))
		return young;

	offset = ((unsigned long)entryp) & ~PAGE_MASK;

	for_each_replica(page, cur) {
		unsigned long *replica_entry =
			(unsigned long *)(page_address(cur) + offset);

		if ((*replica_entry & _PAGE_PRESENT) &&
		    test_bit(_PAGE_BIT_ACCESSED, replica_entry)) {
			if (test_and_clear_bit(_PAGE_BIT_ACCESSED, replica_entry))
				young = 1;
		}
	}

	return young;
}

pte_t pgtable_repl_get_pte(pte_t *ptep)
{
	return __pte(repl_get_entry(ptep));
}

pte_t ptep_get_and_clear(struct mm_struct *mm, unsigned long addr, pte_t *ptep)
{
	pte_t pte = __pte(repl_get_and_clear_entry(ptep));
	page_table_check_pte_clear(mm, addr, pte);
	return pte;
}

void ptep_set_wrprotect(struct mm_struct *mm,
			unsigned long addr, pte_t *ptep)
{
	repl_set_wrprotect_entry(ptep);
}

int ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pte_t *ptep)
{
	return repl_test_and_clear_young_entry(ptep);
}

int pmdp_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long addr, pmd_t *pmdp)
{
	return repl_test_and_clear_young_entry(pmdp);
}

pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm, unsigned long addr,
			      pmd_t *pmdp)
{
	pmd_t pmd = __pmd(repl_get_and_clear_entry(pmdp));
	page_table_check_pmd_clear(mm, addr, pmd);
	return pmd;
}

void pmdp_set_wrprotect(struct mm_struct *mm,
			unsigned long addr, pmd_t *pmdp)
{
	repl_set_wrprotect_entry(pmdp);
}

pmd_t hydra_pmdp_establish(pmd_t *pmdp, pmd_t pmd)
{
	struct page *pmd_page;
	struct page *cur;
	unsigned long offset;
	pmdval_t val;
	pmdval_t flags;
	pmd_t repl_val;

	if (IS_ENABLED(CONFIG_SMP))
		val = pmd_val(xchg(pmdp, pmd));
	else {
		val = pmd_val(*pmdp);
		WRITE_ONCE(*pmdp, pmd);
	}

	flags = pmd_flags(__pmd(val));

	if (!virt_addr_valid(pmdp))
		return __pmd(val);

	pmd_page = virt_to_page(pmdp);

	if (!READ_ONCE(pmd_page->next_replica))
		return __pmd(val);

	if (pmd_present(pmd) && (pmd_trans_huge(pmd) || pmd_leaf(pmd)))
		repl_val = pmd_mkold(pmd);
	else
		repl_val = __pmd(0);

	offset = ((unsigned long)pmdp) & ~PAGE_MASK;

	for_each_replica(pmd_page, cur) {
		pmd_t *replica_entry = (pmd_t *)(page_address(cur) + offset);
		pmd_t old_repl;

		if (IS_ENABLED(CONFIG_SMP))
			old_repl = __pmd(pmd_val(xchg(replica_entry, repl_val)));
		else {
			old_repl = *replica_entry;
			WRITE_ONCE(*replica_entry, repl_val);
		}

		if (pmd_trans_huge(old_repl) || pmd_leaf(old_repl))
			flags |= pmd_flags(old_repl);
	}

	return pmd_set_flags(__pmd(val), flags);
}

pmd_t hydra_get_pmd(pmd_t *pmdp)
{
	return __pmd(repl_get_entry(pmdp));
}

EXPORT_SYMBOL(hydra_get_pmd);
