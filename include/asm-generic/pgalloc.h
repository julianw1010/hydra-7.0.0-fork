#/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_PGALLOC_H
#define __ASM_GENERIC_PGALLOC_H

#ifdef CONFIG_MMU

#define GFP_PGTABLE_KERNEL	(GFP_KERNEL | __GFP_ZERO)
#define GFP_PGTABLE_USER	(GFP_PGTABLE_KERNEL | __GFP_ACCOUNT)

#include <asm/pgtable.h>
#include <linux/hydra_util.h>

static inline pte_t *__pte_alloc_one_kernel_noprof(struct mm_struct *mm)
{
	struct ptdesc *ptdesc;
	struct page *page;

	page = hydra_alloc_pt_page(mm, GFP_PGTABLE_KERNEL, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	if (!pagetable_pte_ctor(mm, ptdesc)) {
		__free_page(page);
		return NULL;
	}
	ptdesc_set_kernel(ptdesc);
	return ptdesc_address(ptdesc);
}

#define __pte_alloc_one_kernel(...)	alloc_hooks(__pte_alloc_one_kernel_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_PTE_ALLOC_ONE_KERNEL
static inline pte_t *pte_alloc_one_kernel_noprof(struct mm_struct *mm)
{
	return __pte_alloc_one_kernel_noprof(mm);
}
#define pte_alloc_one_kernel(...)	alloc_hooks(pte_alloc_one_kernel_noprof(__VA_ARGS__))
#endif

static inline void pte_free_kernel(struct mm_struct *mm, pte_t *pte)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pte);
	struct page *page = ptdesc_page(ptdesc);

	hydra_break_chain(page);
	pagetable_dtor(ptdesc);

	if (hydra_try_return_page(page))
		return;

	pagetable_free(ptdesc);
}

static inline pgtable_t __pte_alloc_one_noprof(struct mm_struct *mm, gfp_t gfp)
{
	struct ptdesc *ptdesc;
	struct page *page;

	page = hydra_alloc_pt_page(mm, gfp, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	if (!pagetable_pte_ctor(mm, ptdesc)) {
		__free_page(page);
		return NULL;
	}

	return ptdesc_page(ptdesc);
}
#define __pte_alloc_one(...)	alloc_hooks(__pte_alloc_one_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_PTE_ALLOC_ONE
static inline pgtable_t pte_alloc_one_noprof(struct mm_struct *mm)
{
	return __pte_alloc_one_noprof(mm, GFP_PGTABLE_USER);
}
#define pte_alloc_one(...)	alloc_hooks(pte_alloc_one_noprof(__VA_ARGS__))
#endif

static inline void pte_free(struct mm_struct *mm, struct page *pte_page)
{
	struct ptdesc *ptdesc = page_ptdesc(pte_page);

	hydra_break_chain(pte_page);
	pagetable_dtor(ptdesc);

	if (hydra_try_return_page(pte_page))
		return;

	pagetable_free(ptdesc);
}

#if CONFIG_PGTABLE_LEVELS > 2

#ifndef __HAVE_ARCH_PMD_ALLOC_ONE
static inline pmd_t *pmd_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	struct ptdesc *ptdesc;
	struct page *page;
	gfp_t gfp = GFP_PGTABLE_USER;

	if (mm == &init_mm)
		gfp = GFP_PGTABLE_KERNEL;

	page = hydra_alloc_pt_page(mm, gfp, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	if (!pagetable_pmd_ctor(mm, ptdesc)) {
		__free_page(page);
		return NULL;
	}

	if (mm == &init_mm)
		ptdesc_set_kernel(ptdesc);

	return ptdesc_address(ptdesc);
}
#define pmd_alloc_one(...)	alloc_hooks(pmd_alloc_one_noprof(__VA_ARGS__))
#endif

#ifndef __HAVE_ARCH_PMD_FREE
static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pmd);
	struct page *page = ptdesc_page(ptdesc);

	BUG_ON((unsigned long)pmd & (PAGE_SIZE - 1));
	hydra_break_chain(page);
	pagetable_dtor(ptdesc);

	if (hydra_try_return_page(page))
		return;

	pagetable_free(ptdesc);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 2 */

#if CONFIG_PGTABLE_LEVELS > 3

static inline pud_t *__pud_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	struct ptdesc *ptdesc;
	struct page *page;
	gfp_t gfp = GFP_PGTABLE_USER;

	if (mm == &init_mm)
		gfp = GFP_PGTABLE_KERNEL;

	page = hydra_alloc_pt_page(mm, gfp | __GFP_ZERO, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	pagetable_pud_ctor(ptdesc);

	if (mm == &init_mm)
		ptdesc_set_kernel(ptdesc);

	return ptdesc_address(ptdesc);
}
#define __pud_alloc_one(...)	alloc_hooks(__pud_alloc_one_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_PUD_ALLOC_ONE
static inline pud_t *pud_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	return __pud_alloc_one_noprof(mm, addr);
}
#define pud_alloc_one(...)	alloc_hooks(pud_alloc_one_noprof(__VA_ARGS__))
#endif

static inline void __pud_free(struct mm_struct *mm, pud_t *pud)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pud);
	struct page *page = ptdesc_page(ptdesc);

	BUG_ON((unsigned long)pud & (PAGE_SIZE - 1));

	if (hydra_try_return_page(page))
		return;

	pagetable_dtor_free(ptdesc);
}

#ifndef __HAVE_ARCH_PUD_FREE
static inline void pud_free(struct mm_struct *mm, pud_t *pud)
{
	__pud_free(mm, pud);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 3 */

#if CONFIG_PGTABLE_LEVELS > 4

static inline p4d_t *__p4d_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	struct ptdesc *ptdesc;
	struct page *page;
	gfp_t gfp = GFP_PGTABLE_USER;

	if (mm == &init_mm)
		gfp = GFP_PGTABLE_KERNEL;

	page = hydra_alloc_pt_page(mm, gfp | __GFP_ZERO, 0);
	if (!page)
		return NULL;

	ptdesc = page_ptdesc(page);
	pagetable_p4d_ctor(ptdesc);

	if (mm == &init_mm)
		ptdesc_set_kernel(ptdesc);

	return ptdesc_address(ptdesc);
}
#define __p4d_alloc_one(...)	alloc_hooks(__p4d_alloc_one_noprof(__VA_ARGS__))

#ifndef __HAVE_ARCH_P4D_ALLOC_ONE
static inline p4d_t *p4d_alloc_one_noprof(struct mm_struct *mm, unsigned long addr)
{
	return __p4d_alloc_one_noprof(mm, addr);
}
#define p4d_alloc_one(...)	alloc_hooks(p4d_alloc_one_noprof(__VA_ARGS__))
#endif

extern p4d_t *repl_p4d_alloc_one(struct mm_struct *mm, unsigned long addr, size_t nid);

static inline void __p4d_free(struct mm_struct *mm, p4d_t *p4d)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(p4d);
	struct page *page = ptdesc_page(ptdesc);

	BUG_ON((unsigned long)p4d & (PAGE_SIZE - 1));

	if (hydra_try_return_page(page))
		return;

	pagetable_dtor_free(ptdesc);
}

#ifndef __HAVE_ARCH_P4D_FREE
static inline void p4d_free(struct mm_struct *mm, p4d_t *p4d)
{
	if (!mm_p4d_folded(mm))
		__p4d_free(mm, p4d);
}
#endif

#endif /* CONFIG_PGTABLE_LEVELS > 4 */

static inline pgd_t *__pgd_alloc_noprof(struct mm_struct *mm, unsigned int order)
{
	gfp_t gfp = GFP_PGTABLE_USER;
	struct ptdesc *ptdesc;

	if (mm == &init_mm)
		gfp = GFP_PGTABLE_KERNEL;

	ptdesc = pagetable_alloc_noprof(gfp, order);
	if (!ptdesc)
		return NULL;

	pagetable_pgd_ctor(ptdesc);

	if (mm == &init_mm)
		ptdesc_set_kernel(ptdesc);

	return ptdesc_address(ptdesc);
}
#define __pgd_alloc(...)	alloc_hooks(__pgd_alloc_noprof(__VA_ARGS__))

static inline void __pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pgd);

	BUG_ON((unsigned long)pgd & (PAGE_SIZE-1));
	pagetable_dtor_free(ptdesc);
}

#ifndef __HAVE_ARCH_PGD_FREE
static inline void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	__pgd_free(mm, pgd);
}
#endif

#endif /* CONFIG_MMU */

#endif /* __ASM_GENERIC_PGALLOC_H */
