#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/huge_mm.h>
#include <linux/pgtable.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

static int hydra_find_master_pmd(struct mm_struct *mm, unsigned long address,
				 int master_node, pmd_t **pmdpp,
				 pmd_t *pmd_valp)
{
	pmd_t *pmd;

	pmd = hydra_walk_to_pmd(mm, address, master_node);
	if (HYDRA_WALK_BAD(pmd))
		return -EFAULT;

	*pmdpp = pmd;
	*pmd_valp = *pmd;
	return 0;
}

static int hydra_find_master_pte(struct mm_struct *mm, unsigned long address,
				 int master_node, pte_t **ptepp,
				 pte_t *pte_valp)
{
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl;

	pmd = hydra_walk_to_pmd(mm, address, master_node);
	if (HYDRA_WALK_BAD(pmd))
		return -EFAULT;

	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
		return -EFAULT;

	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!ptep)
		return -EFAULT;

	*pte_valp = *ptep;
	*ptepp = ptep;
	pte_unmap_unlock(ptep, ptl);

	if (!pte_present(*pte_valp))
		return -ENOENT;

	return 0;
}

int hydra_alloc_replica_pmd(struct mm_struct *mm, pud_t *pud,
			    unsigned long address, size_t owner_node)
{
	spinlock_t *ptl;
	pmd_t *new = pmd_alloc_one(mm, address, pud);
	struct page *new_page, *master_pmd_page = NULL;
	pmd_t *m_pmd;

	if (!new)
		return -ENOMEM;

	new_page = virt_to_page(new);
	smp_wmb();

	ptl = pud_lock(mm, pud);
	if (!pud_present(*pud)) {
		mm_inc_nr_pmds(mm);
		pud_populate(mm, pud, new);

		m_pmd = hydra_walk_to_pmd(mm, address, owner_node);
		if (!HYDRA_WALK_BAD(m_pmd))
			master_pmd_page = virt_to_page(m_pmd);
		if (master_pmd_page && master_pmd_page != new_page)
			hydra_link_page_to_replica_chain(master_pmd_page, new_page);
	} else {
		pagetable_dtor(virt_to_ptdesc(new));
		hydra_pt_account(new_page, -1);

		if (!hydra_try_return_page(new_page))
			__free_page(new_page);
	}
	spin_unlock(ptl);
	return 0;
}

static int hydra_repl_pmd_range(struct mm_struct *mm,
				struct vm_area_struct *vma,
				unsigned long address,
				const int *dst_nodes, int ndst,
				size_t src_node,
				size_t master_node,
				unsigned int fault_flags,
				int order,
				unsigned long *prefetched_out)
{
	pgd_t *master_pgd;
	p4d_t *master_p4d;
	pud_t *master_pud;
	pmd_t *master_pmd_base, *src_pmd_base;
	pmd_t *dst_pmd_base[NUMA_NODE_COUNT];
	spinlock_t *master_ptl;
	unsigned long haddr = address & HPAGE_PMD_MASK;
	unsigned long pud_base, range_start, range_end;
	unsigned long prefetch_count;
	unsigned long start_idx, end_idx, fault_idx, i;
	unsigned long copied = 0, cross = 0;
	bool write_intent;
	long ro = 0, spec = 0;
	int j;

	*prefetched_out = 0;

	write_intent = sysctl_hydra_extended &&
		       (fault_flags & FAULT_FLAG_WRITE) &&
		       vma_is_anonymous(vma) && src_node == master_node;

	if (order > 0) {
		prefetch_count = 1UL << order;
		range_start = haddr & ~((prefetch_count << HPAGE_PMD_SHIFT) - 1);
		range_end = range_start + (prefetch_count << HPAGE_PMD_SHIFT);
	} else {
		range_start = haddr;
		range_end = haddr + HPAGE_PMD_SIZE;
	}

	range_start = max(range_start, vma->vm_start & HPAGE_PMD_MASK);
	range_end = min(range_end, vma->vm_end);

	pud_base = haddr & PUD_MASK;
	range_start = max(range_start, pud_base);
	range_end = min(range_end, pud_base + PUD_SIZE);

	if (range_start >= range_end)
		return -EAGAIN;

	master_pgd = pgd_offset_node(mm, haddr, master_node);
	if (pgd_none(*master_pgd) || pgd_bad(*master_pgd))
		return -EAGAIN;

	master_p4d = p4d_offset(master_pgd, haddr);
	if (p4d_none(*master_p4d) || p4d_bad(*master_p4d))
		return -EAGAIN;

	master_pud = pud_offset(master_p4d, haddr);
	if (pud_none(*master_pud) || pud_bad(*master_pud))
		return -EAGAIN;

	master_pmd_base = pmd_offset(master_pud, pud_base);

	if (src_node == master_node) {
		src_pmd_base = master_pmd_base;
	} else {
		src_pmd_base = hydra_walk_to_pmd(mm, pud_base, src_node);
		if (HYDRA_WALK_BAD(src_pmd_base))
			return -EAGAIN;
	}

	for (j = 0; j < ndst; j++) {
		pgd_t *repl_pgd;
		p4d_t *repl_p4d;
		pud_t *repl_pud;

		repl_pgd = pgd_offset_node(mm, haddr, dst_nodes[j]);
		repl_p4d = p4d_alloc(mm, repl_pgd, haddr);
		if (!repl_p4d)
			return -ENOMEM;

		repl_pud = pud_alloc(mm, repl_p4d, haddr);
		if (!repl_pud)
			return -ENOMEM;

		if (pud_none(*repl_pud)) {
			pmd_t *new_pmd = pmd_alloc_one(mm, haddr, repl_pud);
			struct page *new_pmd_page, *master_pmd_page;
			spinlock_t *pud_ptl;

			if (!new_pmd)
				return -ENOMEM;

			new_pmd_page = virt_to_page(new_pmd);
			master_pmd_page = virt_to_page(master_pmd_base);

			pud_ptl = pud_lock(mm, repl_pud);
			if (!pud_present(*repl_pud)) {
				hydra_link_page_to_replica_chain(master_pmd_page, new_pmd_page);
				mm_inc_nr_pmds(mm);
				pud_populate(mm, repl_pud, new_pmd);
				new_pmd = NULL;
			}
			spin_unlock(pud_ptl);

			if (unlikely(new_pmd))
				hydra_free_chain_node_rcu(new_pmd_page);
		}

		dst_pmd_base[j] = pmd_offset(repl_pud, pud_base);

		{
			struct page *m_pg = virt_to_page(master_pmd_base);
			struct page *r_pg = virt_to_page(dst_pmd_base[j]);

			if (m_pg != r_pg)
				hydra_link_page_to_replica_chain(m_pg, r_pg);
		}
	}

	master_ptl = pmd_lockptr(mm, pmd_offset(master_pud, haddr));
	spin_lock(master_ptl);

	fault_idx = (haddr - pud_base) >> HPAGE_PMD_SHIFT;
	{
		pmd_t faulting_val = master_pmd_base[fault_idx];

		if (!pmd_present(faulting_val) || !pmd_trans_huge(faulting_val))
			goto unlock;
	}

	start_idx = (range_start - pud_base) >> HPAGE_PMD_SHIFT;
	end_idx = (range_end - pud_base) >> HPAGE_PMD_SHIFT;

	for (i = start_idx; i < end_idx; i++) {
		pmd_t src_val = src_pmd_base[i];
		pmd_t ins;
		bool ro_xform = false;
		bool entry_copied = false;

		if (!pmd_present(src_val) || !pmd_trans_huge(src_val))
			continue;

		if (pmd_protnone(src_val))
			continue;

		if (write_intent && pmd_write(src_val) && !pmd_dirty(src_val)) {
			pmd_t newv = pmd_mkdirty(src_val);

			native_set_pmd(&src_pmd_base[i], newv);
			src_val = newv;
			spec++;
			hydra_stats_wr_grant(mm);
		}

		ins = src_val;
		if (sysctl_hydra_extended && pmd_write(ins) &&
		    !pmd_dirty(ins)) {
			ins = pmd_wrprotect(ins);
			ro_xform = true;
		}

		for (j = 0; j < ndst; j++) {
			pmd_t repl_val = dst_pmd_base[j][i];

			if (pmd_present(repl_val) && pmd_trans_huge(repl_val)) {
				unsigned long diff;

				if (!fault_flags)
					continue;

				diff = (pmd_val(repl_val) ^ pmd_val(src_val)) &
				    ~(_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SAVED_DIRTY);

				if (diff & ~_PAGE_RW) {
					struct page *m_pg = virt_to_page(src_pmd_base);
					struct page *r_pg = virt_to_page(dst_pmd_base[j]);

					pr_emerg("HYDRA: replica huge PMD diverged under master PTL: idx %lu src %lx replica %lx m_pg %px (nid %d) next %px r_pg %px (nid %d) next %px\n",
						 i, pmd_val(src_val), pmd_val(repl_val),
						 m_pg, page_to_nid(m_pg),
						 m_pg->next_replica,
						 r_pg, page_to_nid(r_pg),
						 r_pg->next_replica);
					BUG();
				}
				if (diff && pmd_write(repl_val)) {
					hydra_wrprotect_pmd_one(&dst_pmd_base[j][i]);
					continue;
				}
				if (diff && (i == fault_idx || write_intent) &&
				    (fault_flags & FAULT_FLAG_WRITE) &&
				    pmd_write(src_val) && pmd_dirty(src_val)) {
					native_set_pmd(&dst_pmd_base[j][i], src_val);
					hydra_stats_wr_grant(mm);
				}
				continue;
			}

			native_set_pmd(&dst_pmd_base[j][i], ins);
			copied++;
			entry_copied = true;
			if (ro_xform)
				ro++;
		}

		if (entry_copied && !hydra_same_socket(dst_nodes[0], src_node))
			cross++;
	}

	*prefetched_out = copied;
	hydra_stats_copied_pmd(mm, copied);
	hydra_stats_copied_cross(mm, cross);
	hydra_stats_ro_installs(mm, ro);
	hydra_stats_spec_dirty(mm, spec);

unlock:
	spin_unlock(master_ptl);

	return copied > 0 ? 0 : -EAGAIN;
}

static int hydra_repl_try_pmd(struct mm_struct *mm,
			      struct vm_area_struct *vma,
			      unsigned long address,
			      unsigned int flags,
			      const int *dst_nodes, int ndst,
			      size_t src_node,
			      size_t master_node,
			      int order,
			      struct hydra_fill_info *info)
{
	pmd_t *m_pmd;
	pmd_t m_pmdval;
	unsigned long prefetched = 0;
	bool dirty_trigger;
	int ret;

	ret = hydra_find_master_pmd(mm, address, master_node, &m_pmd, &m_pmdval);
	if (ret)
		return -EAGAIN;

	if (!pmd_present(m_pmdval) || !pmd_trans_huge(m_pmdval))
		return -EAGAIN;

	dirty_trigger = (flags & FAULT_FLAG_WRITE) &&
			!pmd_protnone(m_pmdval) &&
			pmd_write(m_pmdval) && !pmd_dirty(m_pmdval);

	if (pmd_protnone(m_pmdval)) {
		ret = __handle_mm_fault(vma, address, flags, 1);
		if (ret & (VM_FAULT_ERROR | VM_FAULT_NOPAGE | VM_FAULT_RETRY | VM_FAULT_COMPLETED))
			return ret;
		m_pmdval = *m_pmd;

		if (!pmd_present(m_pmdval) || !pmd_trans_huge(m_pmdval))
			return -EAGAIN;

		if (pmd_protnone(m_pmdval))
			return -EAGAIN;
	}

	if (((flags & (FAULT_FLAG_WRITE|FAULT_FLAG_UNSHARE)) && !pmd_write(m_pmdval)) ||
	    ((flags & FAULT_FLAG_WRITE) && !pmd_dirty(m_pmdval))) {
		ret = __handle_mm_fault(vma, address, flags, 1);
		if (ret & (VM_FAULT_ERROR | VM_FAULT_NOPAGE | VM_FAULT_RETRY | VM_FAULT_COMPLETED))
			return ret;
		m_pmdval = *m_pmd;
	}

	if (!pmd_present(m_pmdval) || !pmd_trans_huge(m_pmdval) ||
	    pmd_protnone(m_pmdval))
		return -EAGAIN;

	if ((flags & FAULT_FLAG_WRITE) && !pmd_dirty(m_pmdval))
		return -EAGAIN;

	if (dirty_trigger)
		hydra_stats_wr_grant(mm);

	ret = hydra_repl_pmd_range(mm, vma, address, dst_nodes, ndst,
				   src_node, master_node, flags, order,
				   &prefetched);
	if (ret == -ENOMEM)
		return VM_FAULT_OOM;

	if (ret == 0 && info) {
		info->level = HYDRA_PT_PMD;
		info->order = order;
		info->address = address;
		info->start = 0;
		info->end = 0;
	}

	return ret == 0 ? 0 : -EAGAIN;
}

static int hydra_repl_pte_range(struct mm_struct *mm,
				struct vm_area_struct *vma,
				unsigned long pmd_addr,
				unsigned long start_idx,
				unsigned long count,
				const int *dst_nodes, int ndst,
				size_t src_node,
				size_t master_node,
				unsigned long fault_addr,
				unsigned int fault_flags)
{
	pmd_t *master_pmd;
	pmd_t master_pmdval;
	pmd_t *dst_pmd[NUMA_NODE_COUNT];
	pte_t *master_pte;
	pte_t *master_pte_base, *src_pte_base;
	pte_t *dst_pte_base[NUMA_NODE_COUNT];
	spinlock_t *master_ptl, *master_pml;
	unsigned long addr = pmd_addr;
	unsigned long copied = 0, cross = 0;
	bool write_intent;
	long ro = 0, spec = 0;
	int j, ret;

	write_intent = sysctl_hydra_extended &&
		       (fault_flags & FAULT_FLAG_WRITE) &&
		       vma_is_anonymous(vma) && src_node == master_node;

	if (hydra_find_master_pmd(mm, addr, master_node, &master_pmd, &master_pmdval) ||
	    pmd_none(master_pmdval) || pmd_bad(master_pmdval) ||
	    pmd_trans_huge(master_pmdval)) {
		unsigned long fault_va = pmd_addr + (start_idx << PAGE_SHIFT);

		if (!fault_flags)
			return -EAGAIN;

		ret = __handle_mm_fault(vma, fault_va, fault_flags, 1);
		if (ret & (VM_FAULT_ERROR | VM_FAULT_NOPAGE | VM_FAULT_RETRY | VM_FAULT_COMPLETED))
			return (ret & VM_FAULT_RETRY) ? VM_FAULT_RETRY : -EAGAIN;

		if (hydra_find_master_pmd(mm, addr, master_node, &master_pmd, &master_pmdval))
			return -EAGAIN;

		if (pmd_none(master_pmdval) || pmd_bad(master_pmdval) ||
		    pmd_trans_huge(master_pmdval))
			return -EAGAIN;
	}

	master_pte = pte_offset_kernel(master_pmd, addr);
	master_pte_base = (pte_t *)((unsigned long)master_pte & PAGE_MASK);

	for (j = 0; j < ndst; j++) {
		pgd_t *repl_pgd;
		p4d_t *repl_p4d;
		pud_t *repl_pud;
		pmd_t *repl_pmd;

		repl_pgd = pgd_offset_node(mm, addr, dst_nodes[j]);
		repl_p4d = p4d_alloc(mm, repl_pgd, addr);
		if (!repl_p4d)
			return -ENOMEM;

		repl_pud = pud_alloc(mm, repl_p4d, addr);
		if (!repl_pud)
			return -ENOMEM;

		repl_pmd = hydra_repl_pmd_alloc(mm, repl_pud, addr, master_node);
		if (!repl_pmd)
			return -ENOMEM;

		if (pmd_none(*repl_pmd)) {
			pgtable_t new_page = pte_alloc_one(mm, repl_pmd);
			struct page *master_pte_page;

			if (!new_page)
				return -ENOMEM;

			master_pml = pmd_lock(mm, master_pmd);

			if (unlikely(pmd_none(*master_pmd) || pmd_bad(*master_pmd) ||
				     pmd_trans_huge(*master_pmd))) {
				spin_unlock(master_pml);
				hydra_free_chain_node_rcu(new_page);
				return -EAGAIN;
			}

			master_pte = pte_offset_kernel(master_pmd, addr);
			master_pte_base = (pte_t *)((unsigned long)master_pte & PAGE_MASK);
			master_pte_page = virt_to_page(master_pte_base);

			if (likely(pmd_none(*repl_pmd))) {
				hydra_link_page_to_replica_chain(master_pte_page, new_page);
				mm_inc_nr_ptes(mm);
				paravirt_alloc_pte(mm, page_to_pfn(new_page));
				native_set_pmd(repl_pmd,
					__pmd(((pteval_t)page_to_pfn(new_page) << PAGE_SHIFT)
					      | _PAGE_TABLE));
				new_page = NULL;
			}

			spin_unlock(master_pml);

			if (unlikely(new_page))
				hydra_free_chain_node_rcu(new_page);
		}

		dst_pmd[j] = repl_pmd;
	}

	master_pml = pmd_lock(mm, master_pmd);

	if (unlikely(pmd_none(*master_pmd) || pmd_bad(*master_pmd) ||
		     pmd_trans_huge(*master_pmd))) {
		spin_unlock(master_pml);
		return -EAGAIN;
	}

	master_pte = pte_offset_kernel(master_pmd, addr);
	master_pte_base = (pte_t *)((unsigned long)master_pte & PAGE_MASK);

	for (j = 0; j < ndst; j++) {
		if (unlikely(pmd_none(*dst_pmd[j]) || pmd_bad(*dst_pmd[j]) ||
			     pmd_trans_huge(*dst_pmd[j]))) {
			spin_unlock(master_pml);
			return -EAGAIN;
		}

		dst_pte_base[j] = (pte_t *)((unsigned long)pte_offset_kernel(dst_pmd[j], addr) & PAGE_MASK);

		{
			struct page *m_pg = virt_to_page(master_pte_base);
			struct page *r_pg = virt_to_page(dst_pte_base[j]);

			if (m_pg != r_pg)
				hydra_link_page_to_replica_chain(m_pg, r_pg);
		}
	}

	if (src_node == master_node) {
		src_pte_base = master_pte_base;
	} else {
		pmd_t *src_pmd = hydra_walk_to_pmd(mm, addr, src_node);

		if (HYDRA_WALK_BAD(src_pmd) || pmd_none(*src_pmd) ||
		    pmd_bad(*src_pmd) || pmd_trans_huge(*src_pmd)) {
			spin_unlock(master_pml);
			return -EAGAIN;
		}
		src_pte_base = (pte_t *)((unsigned long)pte_offset_kernel(src_pmd, addr) & PAGE_MASK);
	}

	master_ptl = pte_lockptr(mm, master_pmd);
	if (master_ptl != master_pml)
		spin_lock_nested(master_ptl, SINGLE_DEPTH_NESTING);

	{
		unsigned long i;
		unsigned long fault_idx = (fault_addr >> PAGE_SHIFT) &
					  (PTRS_PER_PTE - 1);

		for (i = start_idx; i < start_idx + count; i++) {
			pte_t val = src_pte_base[i];
			pte_t ins;
			bool usable = pte_present(val) && !pte_protnone(val);
			bool ro_xform = false;
			bool entry_copied = false;

			if (write_intent && usable && pte_write(val) &&
			    !pte_dirty(val)) {
				pte_t newv = pte_mkdirty(val);

				native_set_pte(&src_pte_base[i], newv);
				val = newv;
				spec++;
				hydra_stats_wr_grant(mm);
			}

			if (!usable) {
				ins = __pte(0);
			} else {
				ins = val;
				if (sysctl_hydra_extended && pte_write(ins) &&
				    !pte_dirty(ins)) {
					ins = pte_wrprotect(ins);
					ro_xform = true;
				}
			}

			for (j = 0; j < ndst; j++) {
				pte_t repl_cur = dst_pte_base[j][i];

				if (pte_present(repl_cur)) {
					unsigned long diff;

					if (!usable)
						continue;
					if (!fault_flags)
						continue;
					diff = (pte_val(repl_cur) ^ pte_val(val)) &
					       ~(_PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SAVED_DIRTY);
					if (diff & ~_PAGE_RW) {
						struct page *m_pg = virt_to_page(src_pte_base);
						struct page *r_pg = virt_to_page(dst_pte_base[j]);

						pr_emerg("HYDRA: replica PTE diverged under master PTL: idx %lu src %lx replica %lx m_pg %px (nid %d) next %px r_pg %px (nid %d) next %px\n",
							 i, pte_val(val), pte_val(repl_cur),
							 m_pg, page_to_nid(m_pg),
							 m_pg->next_replica,
							 r_pg, page_to_nid(r_pg),
							 r_pg->next_replica);
						BUG();
					}
					if (diff && pte_write(repl_cur)) {
						hydra_wrprotect_pte_one(&dst_pte_base[j][i]);
						continue;
					}
					if (diff && (i == fault_idx || write_intent) &&
					    (fault_flags & FAULT_FLAG_WRITE) &&
					    pte_write(val) && pte_dirty(val)) {
						if (try_cmpxchg((long *)&dst_pte_base[j][i].pte,
								(long *)&repl_cur,
								*(long *)&val))
							hydra_stats_wr_grant(mm);
					}
					continue;
				}

				dst_pte_base[j][i] = ins;
				if (usable) {
					copied++;
					entry_copied = true;
					if (ro_xform)
						ro++;
				}
			}

			if (entry_copied &&
			    !hydra_same_socket(dst_nodes[0], src_node))
				cross++;
		}
	}

	if (master_ptl != master_pml)
		spin_unlock(master_ptl);

	spin_unlock(master_pml);

	hydra_stats_copied_pte(mm, copied);
	hydra_stats_copied_cross(mm, cross);
	hydra_stats_ro_installs(mm, ro);
	hydra_stats_spec_dirty(mm, spec);

	return 0;
}

static int hydra_repl_try_pte(struct vm_fault *vmf, const int *dst_nodes,
			      int ndst, size_t src_node, size_t master_node,
			      int order, struct hydra_fill_info *info)
{
	struct mm_struct *mm = vmf->vma->vm_mm;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long address = vmf->address;
	unsigned long repl_count, range_start, range_end;
	unsigned long pmd_addr, start_idx, count;
	pte_t *ptep, pte_val;
	bool dirty_trigger;
	int ret;

	ret = hydra_find_master_pte(mm, address, master_node, &ptep, &pte_val);
	dirty_trigger = !ret && (vmf->flags & FAULT_FLAG_WRITE) &&
			!pte_protnone(pte_val) &&
			pte_write(pte_val) && !pte_dirty(pte_val);
	if (ret ||
	    ((vmf->flags & (FAULT_FLAG_WRITE|FAULT_FLAG_UNSHARE)) &&
	     !pte_write(pte_val)) ||
	    ((vmf->flags & FAULT_FLAG_WRITE) && !pte_dirty(pte_val)) ||
	    pte_protnone(pte_val)) {
		ret = __handle_mm_fault(vma, address, vmf->flags, 1);
		if (ret & (VM_FAULT_ERROR | VM_FAULT_NOPAGE | VM_FAULT_RETRY | VM_FAULT_COMPLETED))
			return ret;

		ret = hydra_repl_try_pmd(mm, vma, address, vmf->flags,
					 dst_nodes, ndst, src_node,
					 master_node, order, info);
		if (ret != -EAGAIN)
			return ret;

		ret = hydra_find_master_pte(mm, address, master_node, &ptep, &pte_val);
		if (ret)
			return 0;

		if (pte_protnone(pte_val))
			return 0;

		if ((vmf->flags & (FAULT_FLAG_WRITE|FAULT_FLAG_UNSHARE)) &&
		    !pte_write(pte_val))
			return 0;

		if ((vmf->flags & FAULT_FLAG_WRITE) && !pte_dirty(pte_val))
			return 0;
	}

	if (dirty_trigger)
		hydra_stats_wr_grant(mm);

	if (order > 0) {
		repl_count = 1ul << order;
		range_start = address & ~((repl_count << PAGE_SHIFT) - 1);
		range_end = range_start + (repl_count << PAGE_SHIFT);
	} else {
		range_start = address & PAGE_MASK;
		range_end = range_start + PAGE_SIZE;
	}

	range_start = max(range_start, vma->vm_start);
	range_end = min(range_end, vma->vm_end);

	if (range_start >= range_end)
		return 0;

	pmd_addr = range_start & PMD_MASK;
	start_idx = (range_start - pmd_addr) >> PAGE_SHIFT;
	count = (range_end - range_start) >> PAGE_SHIFT;

	ret = hydra_repl_pte_range(mm, vma, pmd_addr, start_idx, count,
				   dst_nodes, ndst, src_node, master_node,
				   address, vmf->flags);

	if (ret == VM_FAULT_RETRY)
		return VM_FAULT_RETRY;
	if (ret == -ENOMEM)
		return VM_FAULT_OOM;
	if (ret == -EINVAL)
		return VM_FAULT_SIGSEGV;
	if (ret == -EAGAIN)
		return 0;

	if (info) {
		info->level = HYDRA_PT_PTE;
		info->order = order;
		info->address = address;
		info->start = range_start;
		info->end = range_end;
	}

	return 0;
}

static int hydra_repl_fill_nodes(struct vm_fault *vmf, const int *dst_nodes,
				 int ndst, size_t src_node, size_t master_node,
				 int order, struct hydra_fill_info *info)
{
	struct mm_struct *mm = vmf->vma->vm_mm;
	int ret;

	ret = hydra_repl_try_pmd(mm, vmf->vma, vmf->address, vmf->flags,
				 dst_nodes, ndst, src_node, master_node,
				 order, info);
	if (ret == -EAGAIN)
		ret = hydra_repl_try_pte(vmf, dst_nodes, ndst, src_node,
					 master_node, order, info);

	return ret;
}

static int hydra_create_replica_pte_table(struct mm_struct *mm,
					  unsigned long addr, int dst_node,
					  size_t master_node)
{
	pmd_t *master_pmd, *repl_pmd;
	pmd_t master_pmdval;
	pgd_t *repl_pgd;
	p4d_t *repl_p4d;
	pud_t *repl_pud;
	spinlock_t *master_pml;
	pgtable_t new_page;
	pte_t *master_pte;
	struct page *master_pte_page;

	if (hydra_find_master_pmd(mm, addr, master_node, &master_pmd, &master_pmdval) ||
	    pmd_none(master_pmdval) || pmd_bad(master_pmdval) ||
	    pmd_trans_huge(master_pmdval))
		return -EAGAIN;

	repl_pgd = pgd_offset_node(mm, addr, dst_node);
	repl_p4d = p4d_alloc(mm, repl_pgd, addr);
	if (!repl_p4d)
		return -ENOMEM;

	repl_pud = pud_alloc(mm, repl_p4d, addr);
	if (!repl_pud)
		return -ENOMEM;

	repl_pmd = hydra_repl_pmd_alloc(mm, repl_pud, addr, master_node);
	if (!repl_pmd)
		return -ENOMEM;

	if (!pmd_none(*repl_pmd))
		return 0;

	new_page = pte_alloc_one(mm, repl_pmd);
	if (!new_page)
		return -ENOMEM;

	master_pml = pmd_lock(mm, master_pmd);

	if (unlikely(pmd_none(*master_pmd) || pmd_bad(*master_pmd) ||
		     pmd_trans_huge(*master_pmd))) {
		spin_unlock(master_pml);
		hydra_free_chain_node_rcu(new_page);
		return -EAGAIN;
	}

	master_pte = pte_offset_kernel(master_pmd, addr);
	master_pte_page = virt_to_page((unsigned long)master_pte & PAGE_MASK);

	if (likely(pmd_none(*repl_pmd))) {
		hydra_link_page_to_replica_chain(master_pte_page, new_page);
		mm_inc_nr_ptes(mm);
		paravirt_alloc_pte(mm, page_to_pfn(new_page));
		native_set_pmd(repl_pmd,
			__pmd(((pteval_t)page_to_pfn(new_page) << PAGE_SHIFT)
			      | _PAGE_TABLE));
		new_page = NULL;
	}

	spin_unlock(master_pml);

	if (unlikely(new_page))
		hydra_free_chain_node_rcu(new_page);

	return 0;
}

void hydra_birth_replica_tables(struct mm_struct *mm, unsigned long address)
{
	size_t master_node;
	pmd_t *master_pmd;
	nodemask_t targets;
	struct page *pmd_page, *pte_page, *cur;
	pte_t *pte;
	void *entry;
	int n;

	if (!sysctl_hydra_extended)
		return;

	entry = xa_load(mm->hydra_pud_owner, address >> PUD_SHIFT);
	if (!entry)
		return;
	master_node = xa_to_value(entry);

	master_pmd = hydra_walk_to_pmd(mm, address, master_node);
	if (HYDRA_WALK_BAD(master_pmd) || pmd_none(*master_pmd) ||
	    pmd_bad(*master_pmd) || pmd_trans_huge(*master_pmd))
		return;

	pmd_page = virt_to_page(master_pmd);
	pte = pte_offset_kernel(master_pmd, address);
	pte_page = virt_to_page(pte);

	nodes_clear(targets);
	rcu_read_lock();
	for (cur = pmd_page->next_replica; cur && cur != pmd_page;
	     cur = cur->next_replica)
		node_set(page_to_nid(cur), targets);
	for (cur = pte_page->next_replica; cur && cur != pte_page;
	     cur = cur->next_replica)
		node_clear(page_to_nid(cur), targets);
	rcu_read_unlock();
	node_clear((int)master_node, targets);

	if (nodes_empty(targets))
		return;

	for_each_node_mask(n, targets)
		hydra_create_replica_pte_table(mm, address, n, master_node);
}

static bool hydra_span_socket_shared(struct mm_struct *mm,
				     unsigned long address, size_t src_node,
				     int level, const nodemask_t *targets)
{
	pmd_t *pmd;
	struct page *page, *cur;
	bool shared = false;

	pmd = hydra_walk_to_pmd(mm, address, src_node);
	if (HYDRA_WALK_BAD(pmd))
		return false;

	if (level == HYDRA_PT_PTE) {
		pmd_t v = READ_ONCE(*pmd);

		if (pmd_none(v) || pmd_trans_huge(v) || pmd_bad(v))
			return false;
		page = pmd_page(v);
	} else {
		page = virt_to_page(pmd);
	}

	rcu_read_lock();
	for (cur = page->next_replica; cur && cur != page;
	     cur = cur->next_replica) {
		if (node_isset(page_to_nid(cur), *targets)) {
			shared = true;
			break;
		}
	}
	rcu_read_unlock();

	return shared;
}

void hydra_backfill_range(struct mm_struct *mm, struct vm_area_struct *vma,
			  const struct hydra_fill_info *info,
			  const int *dst_nodes, int ndst,
			  size_t src_node, size_t master_node)
{
	if (info->level == HYDRA_PT_PMD) {
		unsigned long prefetched;

		hydra_repl_pmd_range(mm, vma, info->address, dst_nodes, ndst,
				     src_node, master_node, 0, info->order,
				     &prefetched);
	} else {
		unsigned long pmd_addr = info->start & PMD_MASK;
		unsigned long start_idx = (info->start - pmd_addr) >> PAGE_SHIFT;
		unsigned long count = (info->end - info->start) >> PAGE_SHIFT;

		hydra_repl_pte_range(mm, vma, pmd_addr, start_idx, count,
				     dst_nodes, ndst, src_node, master_node,
				     info->start, 0);
	}
}

int hydra_repl_fault(struct vm_fault *vmf, int fault_node)
{
	struct mm_struct *mm = vmf->vma->vm_mm;
	size_t repl_node = fault_node;
	size_t master_node = vmf->vma->master_pgd_node;
	struct hydra_fill_info info;
	nodemask_t targets;
	int dsts[2];
	size_t src;
	int rep_node, socket, ndst, order;
	int ret;

	BUG_ON(!mm->lazy_repl_enabled);
	BUG_ON(repl_node == master_node);

	info.level = -1;

	if (!sysctl_hydra_extended) {
		dsts[0] = (int)repl_node;
		ret = hydra_repl_fill_nodes(vmf, dsts, 1, master_node,
					    master_node,
					    sysctl_hydra_repl_order, NULL);
		if (ret == 0) {
			if (hydra_same_socket(repl_node, master_node))
				hydra_stats_fill_local(mm);
			else
				hydra_stats_fill_pull(mm);
		}
		return ret;
	}

	socket = hydra_node_to_socket(repl_node);
	rep_node = hydra_socket_rep[socket];

	if (hydra_same_socket(repl_node, master_node) ||
	    (int)repl_node == rep_node ||
	    (vmf->flags & FAULT_FLAG_WRITE)) {
		dsts[0] = (int)repl_node;
		ndst = 1;
	} else {
		dsts[0] = rep_node;
		dsts[1] = (int)repl_node;
		ndst = 2;
	}

	order = hydra_same_socket(repl_node, master_node) ?
		sysctl_hydra_repl_order : sysctl_hydra_repl_order_pull;

	ret = hydra_repl_fill_nodes(vmf, dsts, ndst, master_node, master_node,
				    order, &info);
	if (ret)
		return ret;

	if (hydra_same_socket(repl_node, master_node))
		hydra_stats_fill_local(mm);
	else
		hydra_stats_fill_pull(mm);

	if (info.level < 0)
		return 0;

	if (vmf->flags & FAULT_FLAG_WRITE)
		return 0;

	targets = hydra_socket_nodes[socket];
	node_clear((int)repl_node, targets);
	node_clear((int)master_node, targets);
	if (!hydra_same_socket(repl_node, master_node))
		node_clear(rep_node, targets);

	if (nodes_empty(targets))
		return 0;

	src = hydra_same_socket(repl_node, master_node) ?
		master_node : (size_t)rep_node;

	if (hydra_span_socket_shared(mm, info.address, src, info.level,
				     &targets)) {
		if (hydra_queue_backfill(mm, socket, src, master_node,
					 &targets, &info))
			hydra_stats_promotion(mm);
	} else {
		hydra_stats_sweep_deferred(mm);
	}

	return 0;
}

static bool hydra_pud_pmd_will_free(unsigned long pud_base, unsigned long pud_end,
				    unsigned long floor, unsigned long ceiling)
{
	if (pud_base < floor)
		return false;
	if (ceiling) {
		ceiling &= PUD_MASK;
		if (!ceiling)
			return false;
	}
	if (pud_end - 1 > ceiling - 1)
		return false;
	return true;
}

void hydra_break_chain_range(struct mm_struct *mm,
				    unsigned long start, unsigned long end,
				    unsigned long floor, unsigned long ceiling)
{
	unsigned long addr, next_pgd, next_p4d, next_pud, next_pmd;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int node;

	for (addr = start & PUD_MASK; addr < end; addr += PUD_SIZE) {
		if (hydra_pud_pmd_will_free(addr, addr + PUD_SIZE, floor, ceiling))
			xa_erase(mm->hydra_pud_owner, addr >> PUD_SHIFT);
	}

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		int prev;

		if (!mm->repl_pgd[node])
			continue;

		for (prev = 0; prev < node; prev++) {
			if (mm->repl_pgd[prev] == mm->repl_pgd[node])
				break;
		}
		if (prev < node)
			continue;

		addr = start;
		pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);

		do {
			next_pgd = pgd_addr_end(addr, end);
			if (pgd_none(*pgd) || pgd_bad(*pgd))
				goto next_pgd_bc;

			p4d = p4d_offset(pgd, addr);
			do {
				next_p4d = p4d_addr_end(addr, next_pgd);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					goto next_p4d_bc;

				pud = pud_offset(p4d, addr);
				do {
					next_pud = pud_addr_end(addr, next_p4d);
					if (pud_none(*pud) || pud_bad(*pud))
						goto next_pud_bc;

					pmd = pmd_offset(pud, addr);
					{
						unsigned long pud_base = addr & PUD_MASK;
						if (hydra_pud_pmd_will_free(pud_base, next_pud, floor, ceiling))
							hydra_break_chain(virt_to_page(pmd));
					}

					do {
						next_pmd = pmd_addr_end(addr, next_pud);
						if (pmd_none(*pmd) ||
						    pmd_trans_huge(*pmd) ||
						    pmd_bad(*pmd))
							goto next_pmd_bc;

						pte = pte_offset_kernel(pmd, addr);
						{
							unsigned long pud_base = addr & PUD_MASK;
							if (hydra_pud_pmd_will_free(pud_base, next_pud, floor, ceiling))
								hydra_break_chain(virt_to_page(pte));
						}
next_pmd_bc:
						addr = next_pmd;
					} while (pmd++, addr != next_pud);
next_pud_bc:
					addr = next_pud;
				} while (pud++, addr != next_p4d);
next_p4d_bc:
				addr = next_p4d;
			} while (p4d++, addr != next_pgd);
next_pgd_bc:
			addr = next_pgd;
		} while (pgd++, addr != end);
	}
}
