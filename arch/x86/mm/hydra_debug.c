#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/gfp.h>
#include <linux/spinlock.h>
#include <linux/nodemask.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/hydra_util.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/tlbflush.h>

static inline int hydra_is_swap_pte(pte_t pte)
{
	return !pte_none(pte) && !pte_present(pte);
}

struct hydra_debug_state {
	struct mutex lock;
	char *buf;
	size_t len;
	size_t cap;
};

static int hdbg_grow(struct hydra_debug_state *st, size_t needed)
{
	size_t new_cap;
	char *new_buf;

	if (st->len + needed <= st->cap)
		return 0;
	new_cap = max(st->cap * 2, st->len + needed + 8192);
	new_buf = kvmalloc(new_cap, GFP_KERNEL);
	if (!new_buf)
		return -ENOMEM;
	if (st->buf) {
		memcpy(new_buf, st->buf, st->len);
		kvfree(st->buf);
	}
	st->buf = new_buf;
	st->cap = new_cap;
	return 0;
}

static __printf(2, 3)
int hdbg_printf(struct hydra_debug_state *st, const char *fmt, ...)
{
	va_list args;
	int ret;

	if (hdbg_grow(st, 512))
		return -ENOMEM;

	va_start(args, fmt);
	ret = vsnprintf(st->buf + st->len, st->cap - st->len, fmt, args);
	va_end(args);

	if (ret >= st->cap - st->len) {
		if (hdbg_grow(st, ret + 1))
			return -ENOMEM;
		va_start(args, fmt);
		ret = vsnprintf(st->buf + st->len, st->cap - st->len, fmt, args);
		va_end(args);
	}

	st->len += ret;
	return 0;
}

static struct mm_struct *hydra_debug_get_mm(pid_t pid)
{
	struct task_struct *task;
	struct mm_struct *mm;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (!task)
		return NULL;

	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

static void decode_flags(unsigned long entry, char *out)
{
	out[0] = (entry & _PAGE_PRESENT)  ? 'P' : '-';
	out[1] = (entry & _PAGE_RW)       ? 'W' : '-';
	out[2] = (entry & _PAGE_USER)     ? 'U' : '-';
	out[3] = (entry & _PAGE_ACCESSED) ? 'A' : '-';
	out[4] = (entry & _PAGE_DIRTY)    ? 'D' : '-';
	out[5] = (entry & _PAGE_PSE)      ? 'H' : '-';
	out[6] = (entry & _PAGE_GLOBAL)   ? 'G' : '-';
	out[7] = (entry & _PAGE_NX)       ? 'X' : '-';
	out[8] = (entry & _PAGE_PROTNONE) ? 'N' : '-';
	out[9] = '\0';
}

static int page_nid_safe(void *entry)
{
	if (!entry || !virt_addr_valid(entry))
		return -1;
	return page_to_nid(virt_to_page(entry));
}

static unsigned long page_pfn_safe(void *entry)
{
	if (!entry || !virt_addr_valid(entry))
		return 0;
	return page_to_pfn(virt_to_page(entry));
}

static int chain_length(struct page *page)
{
	struct page *cur;
	int len = 0;

	if (!page)
		return 0;
	if (!page->next_replica)
		return 1;

	cur = page;
	do {
		len++;
		cur = cur->next_replica;
	} while (cur && cur != page && len <= NUMA_NODE_COUNT + 1);

	return len;
}

static int chain_is_circular(struct page *page)
{
	struct page *cur;
	int count = 0;

	if (!page || !page->next_replica)
		return 0;

	cur = page->next_replica;
	while (cur && cur != page && count < NUMA_NODE_COUNT + 1) {
		count++;
		cur = cur->next_replica;
	}
	return cur == page ? 1 : 0;
}

static int chain_has_duplicate_nids(struct page *page)
{
	struct page *cur;
	nodemask_t seen;
	int count = 0;

	if (!page || !page->next_replica)
		return 0;

	nodes_clear(seen);
	cur = page;
	do {
		int nid = page_to_nid(cur);
		if (node_isset(nid, seen))
			return 1;
		node_set(nid, seen);
		count++;
		cur = cur->next_replica;
	} while (cur && cur != page && count <= NUMA_NODE_COUNT + 1);

	return 0;
}

static void hdbg_translate(struct hydra_debug_state *st,
			   struct mm_struct *mm, unsigned long va)
{
	int node;
	char flags[12];

	hdbg_printf(st, "TRANSLATE\tVA=0x%lx\tREPL_ENABLED=%d\n",
		    va, mm->lazy_repl_enabled ? 1 : 0);

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *pgd_base, *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;
		pte_t *pte;
		unsigned long entry_val;

		pgd_base = mm->repl_pgd[node];
		if (!pgd_base) {
			hdbg_printf(st, "TR_NODE\tNODE=%d\tSTATUS=NO_PGD_BASE\n", node);
			continue;
		}

		hdbg_printf(st, "TR_NODE\tNODE=%d\tPGD_BASE=%px\tPGD_BASE_PA=0x%lx\tPGD_BASE_NID=%d\tIS_PRIMARY=%d\n",
			    node, pgd_base,
			    virt_addr_valid(pgd_base) ? __pa(pgd_base) : 0,
			    page_nid_safe(pgd_base),
			    pgd_base == mm->pgd ? 1 : 0);

		pgd = pgd_offset_pgd(pgd_base, va);
		entry_val = pgd_val(*pgd);
		decode_flags(entry_val, flags);
		hdbg_printf(st, "TR_LEVEL\tNODE=%d\tLEVEL=PGD\tIDX=%lu\tENTRY=0x%lx\tFLAGS=%s\tTBL_PFN=0x%lx\tTBL_NID=%d\n",
			    node, pgd_index(va), entry_val, flags,
			    page_pfn_safe(pgd), page_nid_safe(pgd));

		if (!pgd_present(*pgd)) {
			hdbg_printf(st, "TR_STOP\tNODE=%d\tLEVEL=PGD\tREASON=NOT_PRESENT\n", node);
			continue;
		}

		p4d = p4d_offset(pgd, va);
		entry_val = p4d_val(*p4d);
		decode_flags(entry_val, flags);
		hdbg_printf(st, "TR_LEVEL\tNODE=%d\tLEVEL=P4D\tIDX=%lu\tENTRY=0x%lx\tFLAGS=%s\tTBL_PFN=0x%lx\tTBL_NID=%d\n",
			    node, p4d_index(va), entry_val, flags,
			    page_pfn_safe(p4d), page_nid_safe(p4d));

		if (p4d_none(*p4d) || p4d_bad(*p4d)) {
			hdbg_printf(st, "TR_STOP\tNODE=%d\tLEVEL=P4D\tREASON=%s\n",
				    node, p4d_none(*p4d) ? "NONE" : "BAD");
			continue;
		}

		pud = pud_offset(p4d, va);
		entry_val = pud_val(*pud);
		decode_flags(entry_val, flags);
		hdbg_printf(st, "TR_LEVEL\tNODE=%d\tLEVEL=PUD\tIDX=%lu\tENTRY=0x%lx\tFLAGS=%s\tTBL_PFN=0x%lx\tTBL_NID=%d\tHUGE=%d\n",
			    node, pud_index(va), entry_val, flags,
			    page_pfn_safe(pud), page_nid_safe(pud),
			    pud_leaf(*pud) ? 1 : 0);

		if (pud_none(*pud) || pud_bad(*pud)) {
			hdbg_printf(st, "TR_STOP\tNODE=%d\tLEVEL=PUD\tREASON=%s\n",
				    node, pud_none(*pud) ? "NONE" : "BAD");
			continue;
		}
		if (pud_leaf(*pud)) {
			hdbg_printf(st, "TR_LEAF\tNODE=%d\tLEVEL=PUD\tPHYS=0x%lx\tDATA_PFN=0x%lx\n",
				    node,
				    (pud_pfn(*pud) << PAGE_SHIFT) | (va & ~PUD_MASK),
				    pud_pfn(*pud));
			continue;
		}

		pmd = pmd_offset(pud, va);
		entry_val = pmd_val(*pmd);
		decode_flags(entry_val, flags);
		hdbg_printf(st, "TR_LEVEL\tNODE=%d\tLEVEL=PMD\tIDX=%lu\tENTRY=0x%lx\tFLAGS=%s\tTBL_PFN=0x%lx\tTBL_NID=%d\tHUGE=%d\tTHP=%d\tCHAIN_LEN=%d\n",
			    node, pmd_index(va), entry_val, flags,
			    page_pfn_safe(pmd), page_nid_safe(pmd),
			    pmd_leaf(*pmd) ? 1 : 0,
			    pmd_trans_huge(*pmd) ? 1 : 0,
			    virt_addr_valid(pmd) ? chain_length(virt_to_page(pmd)) : 0);

		if (pmd_none(*pmd)) {
			hdbg_printf(st, "TR_STOP\tNODE=%d\tLEVEL=PMD\tREASON=NONE\n", node);
			continue;
		}
		if (pmd_trans_huge(*pmd) || pmd_leaf(*pmd)) {
			hdbg_printf(st, "TR_LEAF\tNODE=%d\tLEVEL=PMD\tPHYS=0x%lx\tDATA_PFN=0x%lx\n",
				    node,
				    (pmd_pfn(*pmd) << PAGE_SHIFT) | (va & ~PMD_MASK),
				    pmd_pfn(*pmd));
			continue;
		}
		if (pmd_bad(*pmd)) {
			hdbg_printf(st, "TR_STOP\tNODE=%d\tLEVEL=PMD\tREASON=BAD\n", node);
			continue;
		}

		pte = pte_offset_kernel(pmd, va);
		entry_val = pte_val(*pte);
		decode_flags(entry_val, flags);
		hdbg_printf(st, "TR_LEVEL\tNODE=%d\tLEVEL=PTE\tIDX=%lu\tENTRY=0x%lx\tFLAGS=%s\tTBL_PFN=0x%lx\tTBL_NID=%d\tCHAIN_LEN=%d\n",
			    node, pte_index(va), entry_val, flags,
			    page_pfn_safe(pte), page_nid_safe(pte),
			    virt_addr_valid(pte) ? chain_length(virt_to_page(pte)) : 0);

		if (pte_present(*pte)) {
			hdbg_printf(st, "TR_LEAF\tNODE=%d\tLEVEL=PTE\tPHYS=0x%lx\tDATA_PFN=0x%lx\n",
				    node,
				    (pte_pfn(*pte) << PAGE_SHIFT) | (va & ~PAGE_MASK),
				    pte_pfn(*pte));
		} else if (entry_val != 0) {
			hdbg_printf(st, "TR_NONPRESENT\tNODE=%d\tLEVEL=PTE\tENTRY=0x%lx\tSWAP=%d\n",
				    node, entry_val,
				    hydra_is_swap_pte(*pte) ? 1 : 0);
		} else {
			hdbg_printf(st, "TR_STOP\tNODE=%d\tLEVEL=PTE\tREASON=ZERO\n", node);
		}
	}
}

static void hdbg_walk_pmd_range(struct hydra_debug_state *st,
				struct mm_struct *mm,
				struct vm_area_struct *vma,
				unsigned long start, unsigned long end)
{
	unsigned long addr, next;
	int node;
	char flags[12];

	for (addr = start & PMD_MASK; addr < end; addr = next) {
		next = (addr + PMD_SIZE) & PMD_MASK;
		if (next == 0)
			next = end;

		for (node = 0; node < NUMA_NODE_COUNT; node++) {
			pgd_t *pgd;
			p4d_t *p4d;
			pud_t *pud;
			pmd_t *pmd;
			unsigned long entry_val;
			int pte_count = 0, pte_present_count = 0;
			int pte_accessed = 0, pte_dirty_count = 0;

			if (!mm->repl_pgd[node])
				continue;

			pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
			if (!pgd_present(*pgd)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=NO_PGD\n",
					    addr, node);
				continue;
			}

			p4d = p4d_offset(pgd, addr);
			if (p4d_none(*p4d) || p4d_bad(*p4d)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=NO_P4D\n",
					    addr, node);
				continue;
			}

			pud = pud_offset(p4d, addr);
			if (pud_none(*pud)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=NO_PUD\n",
					    addr, node);
				continue;
			}
			if (pud_leaf(*pud)) {
				decode_flags(pud_val(*pud), flags);
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=PUD_HUGE\tENTRY=0x%lx\tFLAGS=%s\tDATA_PFN=0x%lx\n",
					    addr, node, pud_val(*pud), flags, pud_pfn(*pud));
				continue;
			}
			if (pud_bad(*pud)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=PUD_BAD\n",
					    addr, node);
				continue;
			}

			pmd = pmd_offset(pud, addr);
			entry_val = pmd_val(*pmd);

			if (pmd_none(*pmd)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=NONE\tPMD_PFN=0x%lx\tPMD_NID=%d\tCHAIN_LEN=%d\n",
					    addr, node,
					    page_pfn_safe(pmd), page_nid_safe(pmd),
					    virt_addr_valid(pmd) ? chain_length(virt_to_page(pmd)) : 0);
				continue;
			}

			decode_flags(entry_val, flags);

			if (pmd_trans_huge(*pmd) || pmd_leaf(*pmd)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=HUGE\tENTRY=0x%lx\tFLAGS=%s\tDATA_PFN=0x%lx\tPMD_PFN=0x%lx\tPMD_NID=%d\tCHAIN_LEN=%d\n",
					    addr, node, entry_val, flags,
					    pmd_pfn(*pmd),
					    page_pfn_safe(pmd), page_nid_safe(pmd),
					    virt_addr_valid(pmd) ? chain_length(virt_to_page(pmd)) : 0);
				continue;
			}

			if (pmd_bad(*pmd)) {
				hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=BAD\tENTRY=0x%lx\n",
					    addr, node, entry_val);
				continue;
			}

			{
				pte_t *pte_base = pte_offset_kernel(pmd, addr & PMD_MASK);
				int i;
				for (i = 0; i < PTRS_PER_PTE; i++) {
					pte_t val = pte_base[i];
					if (pte_val(val)) {
						pte_count++;
						if (pte_present(val)) {
							pte_present_count++;
							if (pte_young(val))
								pte_accessed++;
							if (pte_val(val) & _PAGE_DIRTY)
								pte_dirty_count++;
						}
					}
				}
			}

			hdbg_printf(st, "WALK_PMD\tVA=0x%lx\tNODE=%d\tSTATUS=TABLE\tENTRY=0x%lx\tFLAGS=%s\tPTE_TBL_PFN=0x%lx\tPMD_PFN=0x%lx\tPMD_NID=%d\tPTE_NID=%d\tPTE_CHAIN_LEN=%d\tPMD_CHAIN_LEN=%d\tNONZERO=%d\tPRESENT=%d\tACCESSED=%d\tDIRTY=%d\n",
				    addr, node, entry_val, flags,
				    entry_val ? (unsigned long)pmd_page_vaddr(*pmd) >> PAGE_SHIFT : 0,
				    page_pfn_safe(pmd), page_nid_safe(pmd),
				    pmd_present(*pmd) && !pmd_leaf(*pmd) ? page_nid_safe((void *)pmd_page_vaddr(*pmd)) : -1,
				    (pmd_present(*pmd) && !pmd_leaf(*pmd) && virt_addr_valid((void *)pmd_page_vaddr(*pmd))) ?
					chain_length(virt_to_page((void *)pmd_page_vaddr(*pmd))) : 0,
				    virt_addr_valid(pmd) ? chain_length(virt_to_page(pmd)) : 0,
				    pte_count, pte_present_count, pte_accessed, pte_dirty_count);
		}
	}
}

static void hdbg_walk(struct hydra_debug_state *st,
		      struct mm_struct *mm,
		      unsigned long range_start, unsigned long range_end,
		      int has_range)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, has_range ? range_start : 0);
	unsigned long walk_end = has_range ? range_end : TASK_SIZE;

	for_each_vma(vmi, vma) {
		unsigned long vstart, vend;

		if (vma->vm_start >= walk_end)
			break;

		vstart = max(vma->vm_start, has_range ? range_start : 0UL);
		vend = min(vma->vm_end, walk_end);

		hdbg_printf(st, "VMA\tSTART=0x%lx\tEND=0x%lx\tFLAGS=0x%lx\tMASTER_NODE=%lu\tPGOFF=0x%lx\n",
			    vma->vm_start, vma->vm_end, vma->vm_flags,
			    vma->master_pgd_node, vma->vm_pgoff);

		hdbg_walk_pmd_range(st, mm, vma, vstart, vend);
	}
}

static void hdbg_report_chain(struct hydra_debug_state *st,
			      struct page *page, const char *level,
			      unsigned long va)
{
	struct page *cur;
	int idx = 0;

	if (!page)
		return;

	hdbg_printf(st, "CHAIN\tVA=0x%lx\tLEVEL=%s\tLEN=%d\tCIRCULAR=%d\tDUP_NIDS=%d\tMASTER_PAGE=%px\tMASTER_PFN=0x%lx\tMASTER_NID=%d\n",
		    va, level,
		    chain_length(page),
		    chain_is_circular(page),
		    chain_has_duplicate_nids(page),
		    page, page_to_pfn(page), page_to_nid(page));

	hdbg_printf(st, "CHAIN_MEMBER\tVA=0x%lx\tORDER=%d\tNID=%d\tPAGE=%px\tPFN=0x%lx\tFROM_CACHE=%d\tOWNER_MM=%px\n",
		    va, idx, page_to_nid(page), page, page_to_pfn(page),
		    PageHydraFromCache(page) ? 1 : 0,
		    page->pt_owner_mm);

	if (!page->next_replica)
		return;

	cur = page->next_replica;
	while (cur && cur != page && idx < NUMA_NODE_COUNT + 1) {
		idx++;
		hdbg_printf(st, "CHAIN_MEMBER\tVA=0x%lx\tORDER=%d\tNID=%d\tPAGE=%px\tPFN=0x%lx\tFROM_CACHE=%d\tOWNER_MM=%px\n",
			    va, idx, page_to_nid(cur), cur, page_to_pfn(cur),
			    PageHydraFromCache(cur) ? 1 : 0,
			    cur->pt_owner_mm);
		cur = cur->next_replica;
	}
}

static void hdbg_chains(struct hydra_debug_state *st,
			struct mm_struct *mm,
			unsigned long range_start, unsigned long range_end,
			int has_range)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, has_range ? range_start : 0);
	unsigned long walk_end = has_range ? range_end : TASK_SIZE;
	int master_node;

	if (!mm->lazy_repl_enabled) {
		hdbg_printf(st, "CHAINS\tSTATUS=REPL_DISABLED\n");
		return;
	}

	for_each_vma(vmi, vma) {
		unsigned long addr, next;
		unsigned long vstart, vend;

		if (vma->vm_start >= walk_end)
			break;

		vstart = max(vma->vm_start, has_range ? range_start : 0UL);
		vend = min(vma->vm_end, walk_end);
		master_node = vma->master_pgd_node;

		for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
			pgd_t *pgd;
			p4d_t *p4d;
			pud_t *pud;
			pmd_t *pmd;

			next = (addr + PMD_SIZE) & PMD_MASK;
			if (next == 0)
				next = vend;

			if (!mm->repl_pgd[master_node])
				continue;

			pgd = pgd_offset_pgd(mm->repl_pgd[master_node], addr);
			if (!pgd_present(*pgd))
				continue;

			p4d = p4d_offset(pgd, addr);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				continue;

			pud = pud_offset(p4d, addr);
			if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
				continue;

			pmd = pmd_offset(pud, addr);
			if (virt_addr_valid(pmd))
				hdbg_report_chain(st, virt_to_page(pmd), "PMD", addr);

			if (pmd_none(*pmd) || pmd_bad(*pmd) || pmd_trans_huge(*pmd) || pmd_leaf(*pmd))
				continue;

			{
				pte_t *pte = pte_offset_kernel(pmd, addr);
				if (virt_addr_valid(pte))
					hdbg_report_chain(st, virt_to_page(pte), "PTE", addr);
			}
		}
	}
}

static void hdbg_check_master_invariant(struct hydra_debug_state *st,
					struct mm_struct *mm,
					unsigned long range_start,
					unsigned long range_end,
					int *fail_count)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int master_node;
	int node;

	for_each_vma(vmi, vma) {
		unsigned long addr, next;
		unsigned long vstart, vend;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
			pmd_t *m_pmd;
			pmd_t m_pmdval;

			next = (addr + PMD_SIZE) & PMD_MASK;
			if (next == 0)
				next = vend;

			m_pmd = hydra_walk_to_pmd(mm, addr, master_node);
			if (HYDRA_WALK_BAD(m_pmd))
				continue;
			m_pmdval = READ_ONCE(*m_pmd);

			for (node = 0; node < NUMA_NODE_COUNT; node++) {
				pmd_t *r_pmd;
				pmd_t r_pmdval;

				if (node == master_node || !mm->repl_pgd[node])
					continue;

				r_pmd = hydra_walk_to_pmd(mm, addr, node);
				if (HYDRA_WALK_BAD(r_pmd))
					continue;
				r_pmdval = READ_ONCE(*r_pmd);

				if (pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval)) {
					if (!pmd_present(m_pmdval) || (!pmd_trans_huge(m_pmdval) && !pmd_leaf(m_pmdval))) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=MASTER_INVARIANT\tLEVEL=PMD\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=REPLICA_HUGE_MASTER_NOT\tR_ENTRY=0x%lx\tM_ENTRY=0x%lx\n",
							    addr, node, master_node,
							    pmd_val(r_pmdval), pmd_val(m_pmdval));
						(*fail_count)++;
						continue;
					}
					if (pmd_pfn(r_pmdval) != pmd_pfn(m_pmdval)) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=MASTER_INVARIANT\tLEVEL=PMD\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=PFN_MISMATCH\tR_PFN=0x%lx\tM_PFN=0x%lx\n",
							    addr, node, master_node,
							    pmd_pfn(r_pmdval), pmd_pfn(m_pmdval));
						(*fail_count)++;
					}
					continue;
				}

				if (pmd_none(r_pmdval) || pmd_bad(r_pmdval))
					continue;

				if (pmd_none(m_pmdval) || pmd_bad(m_pmdval) ||
				    pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval))
					continue;

				{
					pte_t *m_pte_base = pte_offset_kernel(m_pmd, addr & PMD_MASK);
					pte_t *r_pte_base = pte_offset_kernel(r_pmd, addr & PMD_MASK);
					int i;

					for (i = 0; i < PTRS_PER_PTE; i++) {
						pte_t m_val = READ_ONCE(m_pte_base[i]);
						pte_t r_val = READ_ONCE(r_pte_base[i]);
						unsigned long pte_va = (addr & PMD_MASK) + ((unsigned long)i << PAGE_SHIFT);

						if (pte_va < vstart || pte_va >= vend)
							continue;

						if (!pte_present(r_val))
							continue;

						if (!pte_present(m_val)) {
							hdbg_printf(st, "CHECK_FAIL\tTYPE=MASTER_INVARIANT\tLEVEL=PTE\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=REPLICA_PRESENT_MASTER_NOT\tR_ENTRY=0x%lx\tM_ENTRY=0x%lx\n",
								    pte_va, node, master_node,
								    pte_val(r_val), pte_val(m_val));
							(*fail_count)++;
							continue;
						}

						if (pte_pfn(r_val) != pte_pfn(m_val)) {
							hdbg_printf(st, "CHECK_FAIL\tTYPE=MASTER_INVARIANT\tLEVEL=PTE\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=PFN_MISMATCH\tR_PFN=0x%lx\tM_PFN=0x%lx\n",
								    pte_va, node, master_node,
								    pte_pfn(r_val), pte_pfn(m_val));
							(*fail_count)++;
						}
					}
				}
			}
		}
	}
}

static void hdbg_check_chain_integrity(struct hydra_debug_state *st,
				       struct mm_struct *mm,
				       unsigned long range_start,
				       unsigned long range_end,
				       int *fail_count)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int master_node;

	for_each_vma(vmi, vma) {
		unsigned long addr, next;
		unsigned long vstart, vend;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
			pmd_t *pmd;
			struct page *pmd_page, *pte_page;

			next = (addr + PMD_SIZE) & PMD_MASK;
			if (next == 0)
				next = vend;

			pmd = hydra_walk_to_pmd(mm, addr, master_node);
			if (HYDRA_WALK_BAD(pmd))
				continue;

			if (virt_addr_valid(pmd)) {
				pmd_page = virt_to_page(pmd);
				if (pmd_page->next_replica) {
					if (!chain_is_circular(pmd_page)) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_INTEGRITY\tLEVEL=PMD\tVA=0x%lx\tREASON=NOT_CIRCULAR\tLEN=%d\n",
							    addr, chain_length(pmd_page));
						(*fail_count)++;
					}
					if (chain_has_duplicate_nids(pmd_page)) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_INTEGRITY\tLEVEL=PMD\tVA=0x%lx\tREASON=DUPLICATE_NIDS\n", addr);
						(*fail_count)++;
					}
					if (chain_length(pmd_page) > NUMA_NODE_COUNT) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_INTEGRITY\tLEVEL=PMD\tVA=0x%lx\tREASON=CHAIN_TOO_LONG\tLEN=%d\n",
							    addr, chain_length(pmd_page));
						(*fail_count)++;
					}
				}
			}

			if (pmd_none(*pmd) || pmd_bad(*pmd) || pmd_trans_huge(*pmd) || pmd_leaf(*pmd))
				continue;

			{
				pte_t *pte = pte_offset_kernel(pmd, addr);
				if (virt_addr_valid(pte)) {
					pte_page = virt_to_page(pte);
					if (pte_page->next_replica) {
						if (!chain_is_circular(pte_page)) {
							hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_INTEGRITY\tLEVEL=PTE\tVA=0x%lx\tREASON=NOT_CIRCULAR\tLEN=%d\n",
								    addr, chain_length(pte_page));
							(*fail_count)++;
						}
						if (chain_has_duplicate_nids(pte_page)) {
							hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_INTEGRITY\tLEVEL=PTE\tVA=0x%lx\tREASON=DUPLICATE_NIDS\n", addr);
							(*fail_count)++;
						}
						if (chain_length(pte_page) > NUMA_NODE_COUNT) {
							hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_INTEGRITY\tLEVEL=PTE\tVA=0x%lx\tREASON=CHAIN_TOO_LONG\tLEN=%d\n",
								    addr, chain_length(pte_page));
							(*fail_count)++;
						}
					}
				}
			}
		}
	}
}

static void hdbg_check_node_locality(struct hydra_debug_state *st,
				     struct mm_struct *mm,
				     unsigned long range_start,
				     unsigned long range_end,
				     int *fail_count)
{
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, range_start);
		unsigned long addr, next;

		if (!mm->repl_pgd[node] || mm->repl_pgd[node] == mm->pgd)
			continue;

		if (page_nid_safe(mm->repl_pgd[node]) != node) {
			hdbg_printf(st, "CHECK_FAIL\tTYPE=NODE_LOCALITY\tLEVEL=PGD\tNODE=%d\tACTUAL_NID=%d\n",
				    node, page_nid_safe(mm->repl_pgd[node]));
			(*fail_count)++;
		}

		for_each_vma(vmi, vma) {
			unsigned long vstart, vend;

			if (vma->vm_start >= range_end)
				break;

			vstart = max(vma->vm_start, range_start);
			vend = min(vma->vm_end, range_end);

			for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				pmd_t *pmd;
				int actual_nid;

				next = (addr + PMD_SIZE) & PMD_MASK;
				if (next == 0)
					next = vend;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
				if (!pgd_present(*pgd))
					continue;

				p4d = p4d_offset(pgd, addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;

				pud = pud_offset(p4d, addr);
				if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
					continue;

				pmd = pmd_offset(pud, addr);
				actual_nid = page_nid_safe(pmd);
				if (actual_nid >= 0 && actual_nid != node &&
				    actual_nid != (int)vma->master_pgd_node) {
					hdbg_printf(st, "CHECK_FAIL\tTYPE=NODE_LOCALITY\tLEVEL=PMD\tVA=0x%lx\tNODE=%d\tACTUAL_NID=%d\tMASTER_NODE=%lu\n",
						    addr, node, actual_nid, vma->master_pgd_node);
					(*fail_count)++;
				}

				if (pmd_none(*pmd) || pmd_bad(*pmd) || pmd_trans_huge(*pmd) || pmd_leaf(*pmd))
					continue;

				{
					void *pte_page_addr = (void *)pmd_page_vaddr(*pmd);
					actual_nid = page_nid_safe(pte_page_addr);
					if (actual_nid >= 0 && actual_nid != node &&
					    actual_nid != (int)vma->master_pgd_node) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=NODE_LOCALITY\tLEVEL=PTE_TBL\tVA=0x%lx\tNODE=%d\tACTUAL_NID=%d\tMASTER_NODE=%lu\n",
							    addr, node, actual_nid, vma->master_pgd_node);
						(*fail_count)++;
					}
				}
			}
		}
	}
}

static void hdbg_check_pgd_kernel(struct hydra_debug_state *st,
				  struct mm_struct *mm,
				  int *fail_count)
{
	int node;
	pgd_t *primary = mm->pgd;
	unsigned long i;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pgd_t *repl;

		if (!mm->repl_pgd[node] || mm->repl_pgd[node] == primary)
			continue;

		repl = mm->repl_pgd[node];

		for (i = KERNEL_PGD_BOUNDARY; i < PTRS_PER_PGD; i++) {
			if (pgd_val(primary[i]) != pgd_val(repl[i])) {
				hdbg_printf(st, "CHECK_FAIL\tTYPE=PGD_KERNEL\tNODE=%d\tIDX=%lu\tPRIMARY=0x%lx\tREPLICA=0x%lx\n",
					    node, i, pgd_val(primary[i]), pgd_val(repl[i]));
				(*fail_count)++;
			}
		}
	}
}

static void hdbg_check_deposits(struct hydra_debug_state *st,
				struct mm_struct *mm,
				unsigned long range_start,
				unsigned long range_end,
				int *fail_count)
{
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, range_start);

		if (!mm->repl_pgd[node])
			continue;

		for_each_vma(vmi, vma) {
			unsigned long addr, next;
			unsigned long vstart, vend;

			if (vma->vm_start >= range_end)
				break;

			vstart = max(vma->vm_start, range_start);
			vend = min(vma->vm_end, range_end);

			for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				pmd_t *pmd;
				spinlock_t *ptl;
				pgtable_t deposit_head;
				int deposit_count = 0;

				next = (addr + PMD_SIZE) & PMD_MASK;
				if (next == 0)
					next = vend;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
				if (!pgd_present(*pgd))
					continue;
				p4d = p4d_offset(pgd, addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;
				pud = pud_offset(p4d, addr);
				if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
					continue;
				pmd = pmd_offset(pud, addr);

				if (!pmd_trans_huge(*pmd))
					continue;

				if (!virt_addr_valid(pmd))
					continue;

				ptl = pmd_lock(mm, pmd);

				if (!pmd_trans_huge(*pmd)) {
					spin_unlock(ptl);
					continue;
				}

				deposit_head = pmd_huge_pte(mm, pmd);
				if (deposit_head) {
					struct page *dep;
					deposit_count = 1;
					list_for_each_entry(dep, &deposit_head->lru, lru)
						deposit_count++;
				}

				hdbg_printf(st, "DEPOSIT\tVA=0x%lx\tNODE=%d\tPMD_ENTRY=0x%lx\tDEPOSIT_COUNT=%d\tPMD_PFN=0x%lx\tPMD_NID=%d\n",
					    addr, node, pmd_val(*pmd), deposit_count,
					    page_pfn_safe(pmd), page_nid_safe(pmd));

				if (deposit_head) {
					struct page *dep;

					hdbg_printf(st, "DEPOSIT_ENTRY\tVA=0x%lx\tNODE=%d\tDEP_PAGE=%px\tDEP_PFN=0x%lx\tDEP_NID=%d\tDEP_FROM_CACHE=%d\n",
						    addr, node, deposit_head, page_to_pfn(deposit_head),
						    page_to_nid(deposit_head),
						    PageHydraFromCache(deposit_head) ? 1 : 0);

					list_for_each_entry(dep, &deposit_head->lru, lru) {
						hdbg_printf(st, "DEPOSIT_ENTRY\tVA=0x%lx\tNODE=%d\tDEP_PAGE=%px\tDEP_PFN=0x%lx\tDEP_NID=%d\tDEP_FROM_CACHE=%d\n",
							    addr, node, dep, page_to_pfn(dep),
							    page_to_nid(dep),
							    PageHydraFromCache(dep) ? 1 : 0);
					}
				}

				spin_unlock(ptl);
			}
		}
	}
}

static void hdbg_check_cross_node_reachability(struct hydra_debug_state *st,
					       struct mm_struct *mm,
					       unsigned long range_start,
					       unsigned long range_end,
					       int *fail_count)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int master_node;

	for_each_vma(vmi, vma) {
		unsigned long addr, next;
		unsigned long vstart, vend;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
			pmd_t *m_pmd;
			int node;

			next = (addr + PMD_SIZE) & PMD_MASK;
			if (next == 0)
				next = vend;

			m_pmd = hydra_walk_to_pmd(mm, addr, master_node);
			if (HYDRA_WALK_BAD(m_pmd))
				continue;

			if (!virt_addr_valid(m_pmd))
				continue;

			if (pmd_none(*m_pmd) || pmd_bad(*m_pmd))
				continue;

			{
				struct page *m_pmd_page = virt_to_page(m_pmd);
				struct page *chain_cur;
				nodemask_t chain_nodes;

				nodes_clear(chain_nodes);
				hydra_collect_repl_nodes(m_pmd_page, &chain_nodes);

				for (node = 0; node < NUMA_NODE_COUNT; node++) {
					pmd_t *r_pmd;
					struct page *r_pmd_page;
					int found_in_chain;

					if (node == master_node || !mm->repl_pgd[node])
						continue;

					r_pmd = hydra_walk_to_pmd(mm, addr, node);
					if (HYDRA_WALK_BAD(r_pmd))
						continue;

					if (!virt_addr_valid(r_pmd))
						continue;

					r_pmd_page = virt_to_page(r_pmd);
					if (r_pmd_page == m_pmd_page)
						continue;

					found_in_chain = 0;
					chain_cur = m_pmd_page;
					do {
						if (chain_cur == r_pmd_page) {
							found_in_chain = 1;
							break;
						}
						chain_cur = chain_cur->next_replica;
					} while (chain_cur && chain_cur != m_pmd_page);

					if (!found_in_chain) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=ORPHAN_REPLICA\tLEVEL=PMD\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=NOT_IN_MASTER_CHAIN\tR_PAGE=%px\tR_NID=%d\n",
							    addr, node, master_node,
							    r_pmd_page, page_to_nid(r_pmd_page));
						(*fail_count)++;
					}
				}
			}

			if (pmd_trans_huge(*m_pmd) || pmd_leaf(*m_pmd))
				continue;

			{
				pte_t *m_pte = pte_offset_kernel(m_pmd, addr);
				struct page *m_pte_page;
				int node2;

				if (!virt_addr_valid(m_pte))
					continue;

				m_pte_page = virt_to_page(m_pte);

				for (node2 = 0; node2 < NUMA_NODE_COUNT; node2++) {
					pmd_t *r_pmd2;
					pte_t *r_pte;
					struct page *r_pte_page, *chain_cur;
					int found_in_chain;

					if (node2 == master_node || !mm->repl_pgd[node2])
						continue;

					r_pmd2 = hydra_walk_to_pmd(mm, addr, node2);
					if (HYDRA_WALK_BAD(r_pmd2))
						continue;
					if (pmd_none(*r_pmd2) || pmd_bad(*r_pmd2) ||
					    pmd_trans_huge(*r_pmd2) || pmd_leaf(*r_pmd2))
						continue;

					r_pte = pte_offset_kernel(r_pmd2, addr);
					if (!virt_addr_valid(r_pte))
						continue;

					r_pte_page = virt_to_page(r_pte);
					if (r_pte_page == m_pte_page)
						continue;

					found_in_chain = 0;
					chain_cur = m_pte_page;
					do {
						if (chain_cur == r_pte_page) {
							found_in_chain = 1;
							break;
						}
						chain_cur = chain_cur->next_replica;
					} while (chain_cur && chain_cur != m_pte_page);

					if (!found_in_chain) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=ORPHAN_REPLICA\tLEVEL=PTE\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=NOT_IN_MASTER_CHAIN\tR_PAGE=%px\tR_NID=%d\n",
							    addr, node2, master_node,
							    r_pte_page, page_to_nid(r_pte_page));
						(*fail_count)++;
					}
				}
			}
		}
	}
}

static void hdbg_check_pte_subset(struct hydra_debug_state *st,
				  struct mm_struct *mm,
				  unsigned long range_start,
				  unsigned long range_end,
				  int *fail_count)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int master_node;

	for_each_vma(vmi, vma) {
		unsigned long addr, next;
		unsigned long vstart, vend;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
			pmd_t *m_pmd;
			struct page *m_pmd_page;
			nodemask_t pmd_chain_nodes, pte_chain_nodes;

			next = (addr + PMD_SIZE) & PMD_MASK;
			if (next == 0)
				next = vend;

			m_pmd = hydra_walk_to_pmd(mm, addr, master_node);
			if (HYDRA_WALK_BAD(m_pmd))
				continue;
			if (!virt_addr_valid(m_pmd))
				continue;
			if (pmd_none(*m_pmd) || pmd_bad(*m_pmd) ||
			    pmd_trans_huge(*m_pmd) || pmd_leaf(*m_pmd))
				continue;

			m_pmd_page = virt_to_page(m_pmd);
			nodes_clear(pmd_chain_nodes);
			hydra_collect_repl_nodes(m_pmd_page, &pmd_chain_nodes);

			{
				pte_t *m_pte = pte_offset_kernel(m_pmd, addr);
				struct page *m_pte_page;

				if (!virt_addr_valid(m_pte))
					continue;

				m_pte_page = virt_to_page(m_pte);
				nodes_clear(pte_chain_nodes);
				hydra_collect_repl_nodes(m_pte_page, &pte_chain_nodes);

				if (!nodes_subset(pte_chain_nodes, pmd_chain_nodes)) {
					hdbg_printf(st, "CHECK_FAIL\tTYPE=CHAIN_SUBSET\tLEVEL=PTE_IN_PMD\tVA=0x%lx\tREASON=PTE_NODES_NOT_SUBSET_OF_PMD\tPMD_NODES=0x%lx\tPTE_NODES=0x%lx\n",
						    addr,
						    nodes_addr(pmd_chain_nodes)[0],
						    nodes_addr(pte_chain_nodes)[0]);
					(*fail_count)++;
				}
			}
		}
	}
}

static void hdbg_summary(struct hydra_debug_state *st,
			 struct mm_struct *mm)
{
	int node;
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);
	int vma_count = 0;
	long pgtables_bytes;

	pgtables_bytes = atomic_long_read(&mm->pgtables_bytes);

	hdbg_printf(st, "SUMMARY\tPID_MM=%px\tREPL_ENABLED=%d\tPGTABLES_BYTES=%ld\tPRIMARY_PGD=%px\n",
		    mm, mm->lazy_repl_enabled ? 1 : 0,
		    pgtables_bytes, mm->pgd);

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		hdbg_printf(st, "PGD_NODE\tNODE=%d\tPGD=%px\tPA=0x%lx\tNID=%d\tIS_PRIMARY=%d\n",
			    node,
			    mm->repl_pgd[node],
			    mm->repl_pgd[node] && virt_addr_valid(mm->repl_pgd[node]) ?
				__pa(mm->repl_pgd[node]) : 0,
			    page_nid_safe(mm->repl_pgd[node]),
			    mm->repl_pgd[node] == mm->pgd ? 1 : 0);
	}

	for_each_vma(vmi, vma) {
		vma_count++;
		hdbg_printf(st, "VMA_INFO\tIDX=%d\tSTART=0x%lx\tEND=0x%lx\tSIZE=0x%lx\tFLAGS=0x%lx\tMASTER_NODE=%lu\tPUD_SPAN=%lu\n",
			    vma_count,
			    vma->vm_start, vma->vm_end,
			    vma->vm_end - vma->vm_start,
			    vma->vm_flags,
			    vma->master_pgd_node,
			    ((vma->vm_end - 1) >> PUD_SHIFT) - (vma->vm_start >> PUD_SHIFT) + 1);
	}

	hdbg_printf(st, "VMA_TOTAL\tCOUNT=%d\n", vma_count);

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		int pgd_present_count = 0, pud_present_count = 0;
		int pmd_present_count = 0, pmd_huge_count = 0;
		int pte_tbl_count = 0, pte_present_total = 0;
		unsigned long addr;

		if (!mm->repl_pgd[node])
			continue;

		vma_iter_init(&vmi, mm, 0);

		for_each_vma(vmi, vma) {
			unsigned long vend = vma->vm_end;

			for (addr = vma->vm_start & PMD_MASK; addr < vend;
			     addr = (addr + PMD_SIZE) & PMD_MASK) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				pmd_t *pmd;

				if (addr == 0)
					break;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
				if (!pgd_present(*pgd))
					continue;

				if ((addr & PGDIR_MASK) == addr)
					pgd_present_count++;

				p4d = p4d_offset(pgd, addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;

				pud = pud_offset(p4d, addr);
				if (pud_none(*pud) || pud_leaf(*pud))
					continue;
				if (pud_bad(*pud))
					continue;

				if ((addr & PUD_MASK) == addr)
					pud_present_count++;

				pmd = pmd_offset(pud, addr);
				if (pmd_none(*pmd))
					continue;

				pmd_present_count++;

				if (pmd_trans_huge(*pmd) || pmd_leaf(*pmd)) {
					pmd_huge_count++;
					continue;
				}

				if (pmd_bad(*pmd))
					continue;

				pte_tbl_count++;
				{
					pte_t *base = pte_offset_kernel(pmd, addr & PMD_MASK);
					int i;
					for (i = 0; i < PTRS_PER_PTE; i++) {
						if (pte_present(base[i]))
							pte_present_total++;
					}
				}
			}
		}

		hdbg_printf(st, "NODE_STATS\tNODE=%d\tPGD_ENTRIES=%d\tPUD_ENTRIES=%d\tPMD_ENTRIES=%d\tPMD_HUGE=%d\tPTE_TABLES=%d\tPTE_PRESENT=%d\n",
			    node, pgd_present_count, pud_present_count,
			    pmd_present_count, pmd_huge_count,
			    pte_tbl_count, pte_present_total);
	}
}

static void hdbg_check_vma_master_node(struct hydra_debug_state *st,
				       struct mm_struct *mm,
				       int *fail_count)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);

	for_each_vma(vmi, vma) {
		if (vma->master_pgd_node >= NUMA_NODE_COUNT) {
			hdbg_printf(st, "CHECK_FAIL\tTYPE=VMA_MASTER_NODE\tVA=0x%lx\tEND=0x%lx\tMASTER_NODE=%lu\tREASON=OUT_OF_RANGE\n",
				    vma->vm_start, vma->vm_end, vma->master_pgd_node);
			(*fail_count)++;
		} else if (!mm->repl_pgd[vma->master_pgd_node]) {
			hdbg_printf(st, "CHECK_FAIL\tTYPE=VMA_MASTER_NODE\tVA=0x%lx\tEND=0x%lx\tMASTER_NODE=%lu\tREASON=NO_PGD_FOR_NODE\n",
				    vma->vm_start, vma->vm_end, vma->master_pgd_node);
			(*fail_count)++;
		}
	}
}

static void hdbg_check_stale_replicas(struct hydra_debug_state *st,
				      struct mm_struct *mm,
				      unsigned long range_start,
				      unsigned long range_end,
				      int *fail_count)
{
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, range_start);
		unsigned long prev_vma_end = range_start;

		if (!mm->repl_pgd[node])
			continue;

		for_each_vma(vmi, vma) {
			unsigned long gap_start, gap_end, addr, next;
			unsigned long check_start, check_end;

			if (vma->vm_start >= range_end)
				break;

			gap_start = prev_vma_end;
			gap_end = min(vma->vm_start, range_end);
			prev_vma_end = vma->vm_end;

			check_start = (gap_start + PMD_SIZE - 1) & PMD_MASK;
			check_end = gap_end & PMD_MASK;

			for (addr = check_start; addr < check_end; addr = next) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				pmd_t *pmd;

				next = (addr + PMD_SIZE) & PMD_MASK;
				if (next == 0)
					break;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
				if (!pgd_present(*pgd))
					continue;
				p4d = p4d_offset(pgd, addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;
				pud = pud_offset(p4d, addr);
				if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
					continue;
				pmd = pmd_offset(pud, addr);

				if (pmd_present(*pmd)) {
					hdbg_printf(st, "CHECK_FAIL\tTYPE=STALE_UNMAPPED\tNODE=%d\tVA=0x%lx\tPMD_ENTRY=0x%lx\tREASON=PMD_PRESENT_IN_VMA_GAP\n",
						    node, addr, pmd_val(*pmd));
					(*fail_count)++;
				}
			}
		}
	}
}

static void hdbg_pte_dump(struct hydra_debug_state *st,
			  struct mm_struct *mm,
			  unsigned long range_start,
			  unsigned long range_end)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int node;

	for_each_vma(vmi, vma) {
		unsigned long pmd_addr, next_pmd;
		unsigned long vstart, vend;
		int master_node;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (pmd_addr = vstart & PMD_MASK; pmd_addr < vend; pmd_addr = next_pmd) {
			next_pmd = (pmd_addr + PMD_SIZE) & PMD_MASK;
			if (next_pmd == 0)
				next_pmd = vend;

			for (node = 0; node < NUMA_NODE_COUNT; node++) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				pmd_t *pmd;
				pmd_t pmdval;
				char flags[12];

				if (!mm->repl_pgd[node])
					continue;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], pmd_addr);
				if (!pgd_present(*pgd))
					continue;
				p4d = p4d_offset(pgd, pmd_addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;
				pud = pud_offset(p4d, pmd_addr);
				if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
					continue;

				pmd = pmd_offset(pud, pmd_addr);
				pmdval = READ_ONCE(*pmd);

				if (pmd_none(pmdval))
					continue;

				if (pmd_trans_huge(pmdval) || pmd_leaf(pmdval)) {
					decode_flags(pmd_val(pmdval), flags);
					hdbg_printf(st, "PTE_DUMP\tVA=0x%lx\tNODE=%d\tIS_MASTER=%d\tLEVEL=PMD_HUGE\tENTRY=0x%lx\tPFN=0x%lx\tFLAGS=%s\tPMD_PAGE_PFN=0x%lx\tPMD_PAGE_NID=%d\n",
						    pmd_addr, node,
						    node == master_node ? 1 : 0,
						    pmd_val(pmdval),
						    pmd_pfn(pmdval),
						    flags,
						    page_pfn_safe(pmd),
						    page_nid_safe(pmd));
					continue;
				}

				if (pmd_bad(pmdval))
					continue;

				{
					pte_t *pte_base = pte_offset_kernel(pmd, pmd_addr & PMD_MASK);
					unsigned long page_va;
					int i;
					int start_i = 0, end_i = PTRS_PER_PTE;

					if ((pmd_addr & PMD_MASK) < vstart)
						start_i = (vstart - (pmd_addr & PMD_MASK)) >> PAGE_SHIFT;
					if ((pmd_addr & PMD_MASK) + PMD_SIZE > vend)
						end_i = (vend - (pmd_addr & PMD_MASK)) >> PAGE_SHIFT;

					for (i = start_i; i < end_i; i++) {
						pte_t val = READ_ONCE(pte_base[i]);
						unsigned long raw = pte_val(val);

						page_va = (pmd_addr & PMD_MASK) + ((unsigned long)i << PAGE_SHIFT);

						if (!raw)
							continue;

						decode_flags(raw, flags);
						hdbg_printf(st, "PTE_DUMP\tVA=0x%lx\tNODE=%d\tIS_MASTER=%d\tLEVEL=PTE\tIDX=%d\tENTRY=0x%lx\tPFN=0x%lx\tFLAGS=%s\tPRESENT=%d\tSWAP=%d\tPROTNONE=%d\tPTE_PAGE_PFN=0x%lx\tPTE_PAGE_NID=%d\n",
							    page_va, node,
							    node == master_node ? 1 : 0,
							    i, raw,
							    pte_present(val) ? pte_pfn(val) : 0,
							    flags,
							    pte_present(val) ? 1 : 0,
							    hydra_is_swap_pte(val) ? 1 : 0,
							    pte_protnone(val) ? 1 : 0,
							    page_pfn_safe(pte_base + i),
							    page_nid_safe(pte_base + i));
					}
				}
			}
		}
	}
}

static void hdbg_cross_compare(struct hydra_debug_state *st,
			       struct mm_struct *mm,
			       unsigned long range_start,
			       unsigned long range_end)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int master_node;
	int diffs = 0, matches = 0, master_only = 0, replica_only = 0;

	if (!mm->lazy_repl_enabled) {
		hdbg_printf(st, "CROSS_COMPARE\tSTATUS=REPL_DISABLED\n");
		return;
	}

	for_each_vma(vmi, vma) {
		unsigned long pmd_addr, next_pmd;
		unsigned long vstart, vend;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (pmd_addr = vstart & PMD_MASK; pmd_addr < vend; pmd_addr = next_pmd) {
			pmd_t *m_pmd;
			pmd_t m_pmdval;
			int node;

			next_pmd = (pmd_addr + PMD_SIZE) & PMD_MASK;
			if (next_pmd == 0)
				next_pmd = vend;

			m_pmd = hydra_walk_to_pmd(mm, pmd_addr, master_node);
			if (HYDRA_WALK_BAD(m_pmd))
				continue;
			m_pmdval = READ_ONCE(*m_pmd);

			for (node = 0; node < NUMA_NODE_COUNT; node++) {
				pmd_t *r_pmd;
				pmd_t r_pmdval;
				char m_flags[12], r_flags[12];

				if (node == master_node || !mm->repl_pgd[node])
					continue;

				r_pmd = hydra_walk_to_pmd(mm, pmd_addr, node);
				if (HYDRA_WALK_BAD(r_pmd))
					continue;
				r_pmdval = READ_ONCE(*r_pmd);

				if ((pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval)) &&
				    (pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval))) {
					if (pmd_pfn(m_pmdval) != pmd_pfn(r_pmdval)) {
						decode_flags(pmd_val(m_pmdval), m_flags);
						decode_flags(pmd_val(r_pmdval), r_flags);
						hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PMD_HUGE\tTYPE=PFN_DIFF\tM_PFN=0x%lx\tR_PFN=0x%lx\tM_FLAGS=%s\tR_FLAGS=%s\n",
							    pmd_addr, node, master_node,
							    pmd_pfn(m_pmdval), pmd_pfn(r_pmdval),
							    m_flags, r_flags);
						diffs++;
					} else if (pmd_val(m_pmdval) != pmd_val(r_pmdval)) {
						decode_flags(pmd_val(m_pmdval), m_flags);
						decode_flags(pmd_val(r_pmdval), r_flags);
						hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PMD_HUGE\tTYPE=FLAGS_DIFF\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\tM_FLAGS=%s\tR_FLAGS=%s\n",
							    pmd_addr, node, master_node,
							    pmd_val(m_pmdval), pmd_val(r_pmdval),
							    m_flags, r_flags);
						diffs++;
					} else {
						matches++;
					}
					continue;
				}

				if ((pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval)) &&
				    !(pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval))) {
					hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PMD\tTYPE=MASTER_HUGE_REPLICA_NOT\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\tR_IS_NONE=%d\tR_IS_TABLE=%d\n",
						    pmd_addr, node, master_node,
						    pmd_val(m_pmdval), pmd_val(r_pmdval),
						    pmd_none(r_pmdval) ? 1 : 0,
						    (!pmd_none(r_pmdval) && !pmd_bad(r_pmdval) && !pmd_trans_huge(r_pmdval) && !pmd_leaf(r_pmdval)) ? 1 : 0);
					diffs++;
					continue;
				}

				if (!(pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval)) &&
				    (pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval))) {
					decode_flags(pmd_val(m_pmdval), m_flags);
					decode_flags(pmd_val(r_pmdval), r_flags);
					hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PMD\tTYPE=REPLICA_HUGE_MASTER_NOT\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\tM_IS_NONE=%d\tM_IS_TABLE=%d\tM_FLAGS=%s\tR_FLAGS=%s\tR_PFN=0x%lx\n",
						    pmd_addr, node, master_node,
						    pmd_val(m_pmdval), pmd_val(r_pmdval),
						    pmd_none(m_pmdval) ? 1 : 0,
						    (!pmd_none(m_pmdval) && !pmd_bad(m_pmdval) && !pmd_trans_huge(m_pmdval) && !pmd_leaf(m_pmdval)) ? 1 : 0,
						    m_flags, r_flags,
						    pmd_pfn(r_pmdval));
					diffs++;
					continue;
				}

				if (pmd_none(m_pmdval) && !pmd_none(r_pmdval)) {
					decode_flags(pmd_val(r_pmdval), r_flags);
					hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PMD\tTYPE=MASTER_NONE_REPLICA_PRESENT\tR_ENTRY=0x%lx\tR_FLAGS=%s\tR_IS_TABLE=%d\n",
						    pmd_addr, node, master_node,
						    pmd_val(r_pmdval), r_flags,
						    (!pmd_bad(r_pmdval) && !pmd_trans_huge(r_pmdval) && !pmd_leaf(r_pmdval)) ? 1 : 0);
					diffs++;
					replica_only++;
					continue;
				}

				if (pmd_none(m_pmdval) || pmd_bad(m_pmdval) ||
				    pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval))
					continue;

				if (pmd_none(r_pmdval) || pmd_bad(r_pmdval))
					continue;

				if (pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval))
					continue;

				{
					pte_t *m_base = pte_offset_kernel(m_pmd, pmd_addr & PMD_MASK);
					pte_t *r_base = pte_offset_kernel(r_pmd, pmd_addr & PMD_MASK);
					int i;
					int start_i = 0, end_i = PTRS_PER_PTE;

					if ((pmd_addr & PMD_MASK) < vstart)
						start_i = (vstart - (pmd_addr & PMD_MASK)) >> PAGE_SHIFT;
					if ((pmd_addr & PMD_MASK) + PMD_SIZE > vend)
						end_i = (vend - (pmd_addr & PMD_MASK)) >> PAGE_SHIFT;

					for (i = start_i; i < end_i; i++) {
						pte_t mv = READ_ONCE(m_base[i]);
						pte_t rv = READ_ONCE(r_base[i]);
						unsigned long page_va = (pmd_addr & PMD_MASK) + ((unsigned long)i << PAGE_SHIFT);

						if (!pte_val(mv) && !pte_val(rv))
							continue;

						if (pte_present(mv) && pte_present(rv)) {
							if (pte_pfn(mv) != pte_pfn(rv)) {
								decode_flags(pte_val(mv), m_flags);
								decode_flags(pte_val(rv), r_flags);
								hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PTE\tTYPE=PFN_DIFF\tM_PFN=0x%lx\tR_PFN=0x%lx\tM_FLAGS=%s\tR_FLAGS=%s\n",
									    page_va, node, master_node,
									    pte_pfn(mv), pte_pfn(rv),
									    m_flags, r_flags);
								diffs++;
							} else if (pte_val(mv) != pte_val(rv)) {
								decode_flags(pte_val(mv), m_flags);
								decode_flags(pte_val(rv), r_flags);
								hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PTE\tTYPE=FLAGS_DIFF\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\tM_FLAGS=%s\tR_FLAGS=%s\n",
									    page_va, node, master_node,
									    pte_val(mv), pte_val(rv),
									    m_flags, r_flags);
								diffs++;
							} else {
								matches++;
							}
						} else if (pte_present(mv) && !pte_val(rv)) {
							master_only++;
						} else if (!pte_val(mv) && pte_present(rv)) {
							decode_flags(pte_val(rv), r_flags);
							hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PTE\tTYPE=REPLICA_ONLY\tR_ENTRY=0x%lx\tR_FLAGS=%s\n",
								    page_va, node, master_node,
								    pte_val(rv), r_flags);
							replica_only++;
							diffs++;
						} else if (pte_present(rv) && !pte_present(mv) && pte_val(mv)) {
							decode_flags(pte_val(mv), m_flags);
							decode_flags(pte_val(rv), r_flags);
							hdbg_printf(st, "CROSS_DIFF\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tLEVEL=PTE\tTYPE=MASTER_NONPRESENT_REPLICA_PRESENT\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\tM_FLAGS=%s\tR_FLAGS=%s\n",
								    page_va, node, master_node,
								    pte_val(mv), pte_val(rv),
								    m_flags, r_flags);
							diffs++;
						}
					}
				}
			}
		}
	}

	hdbg_printf(st, "CROSS_SUMMARY\tMATCHES=%d\tDIFFS=%d\tMASTER_ONLY=%d\tREPLICA_ONLY=%d\n",
		    matches, diffs, master_only, replica_only);
}

static void hdbg_check_owner_mm(struct hydra_debug_state *st,
				struct mm_struct *mm,
				unsigned long range_start,
				unsigned long range_end,
				int *fail_count)
{
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, range_start);

		if (!mm->repl_pgd[node])
			continue;

		{
			struct page *pgd_page;
			if (virt_addr_valid(mm->repl_pgd[node])) {
				pgd_page = virt_to_page(mm->repl_pgd[node]);
				if (pgd_page->pt_owner_mm && pgd_page->pt_owner_mm != mm) {
					hdbg_printf(st, "CHECK_FAIL\tTYPE=OWNER_MM\tLEVEL=PGD\tNODE=%d\tEXPECTED=%px\tACTUAL=%px\n",
						    node, mm, pgd_page->pt_owner_mm);
					(*fail_count)++;
				}
				hdbg_printf(st, "OWNER_MM\tLEVEL=PGD\tNODE=%d\tPAGE=%px\tOWNER=%px\tEXPECTED=%px\tMATCH=%d\n",
					    node, pgd_page, pgd_page->pt_owner_mm, mm,
					    pgd_page->pt_owner_mm == mm ? 1 : 0);
			}
		}

		for_each_vma(vmi, vma) {
			unsigned long addr, next;
			unsigned long vstart, vend;

			if (vma->vm_start >= range_end)
				break;

			vstart = max(vma->vm_start, range_start);
			vend = min(vma->vm_end, range_end);

			for (addr = vstart & PMD_MASK; addr < vend; addr = next) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				pmd_t *pmd;
				struct page *pg;

				next = (addr + PMD_SIZE) & PMD_MASK;
				if (next == 0)
					next = vend;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
				if (!pgd_present(*pgd))
					continue;
				p4d = p4d_offset(pgd, addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;
				pud = pud_offset(p4d, addr);
				if (pud_none(*pud) || pud_bad(*pud) || pud_leaf(*pud))
					continue;

				pmd = pmd_offset(pud, addr);
				if (!virt_addr_valid(pmd))
					continue;

				pg = virt_to_page(pmd);
				if (pg->pt_owner_mm && pg->pt_owner_mm != mm) {
					hdbg_printf(st, "CHECK_FAIL\tTYPE=OWNER_MM\tLEVEL=PMD_TBL\tNODE=%d\tVA=0x%lx\tPAGE=%px\tEXPECTED=%px\tACTUAL=%px\n",
						    node, addr, pg, mm, pg->pt_owner_mm);
					(*fail_count)++;
				}

				if (pmd_none(*pmd) || pmd_bad(*pmd) ||
				    pmd_trans_huge(*pmd) || pmd_leaf(*pmd))
					continue;

				{
					void *pte_tbl = (void *)pmd_page_vaddr(*pmd);
					struct page *pte_pg;

					if (!virt_addr_valid(pte_tbl))
						continue;

					pte_pg = virt_to_page(pte_tbl);
					if (pte_pg->pt_owner_mm && pte_pg->pt_owner_mm != mm) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=OWNER_MM\tLEVEL=PTE_TBL\tNODE=%d\tVA=0x%lx\tPAGE=%px\tEXPECTED=%px\tACTUAL=%px\n",
							    node, addr, pte_pg, mm, pte_pg->pt_owner_mm);
						(*fail_count)++;
					}
				}
			}
		}
	}
}

static void hdbg_check_wrprotect_consistency(struct hydra_debug_state *st,
					     struct mm_struct *mm,
					     unsigned long range_start,
					     unsigned long range_end,
					     int *fail_count)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, range_start);
	int master_node;

	if (!mm->lazy_repl_enabled)
		return;

	for_each_vma(vmi, vma) {
		unsigned long pmd_addr, next_pmd;
		unsigned long vstart, vend;
		int node;

		if (vma->vm_start >= range_end)
			break;

		vstart = max(vma->vm_start, range_start);
		vend = min(vma->vm_end, range_end);
		master_node = vma->master_pgd_node;

		for (pmd_addr = vstart & PMD_MASK; pmd_addr < vend; pmd_addr = next_pmd) {
			pmd_t *m_pmd;
			pmd_t m_pmdval;

			next_pmd = (pmd_addr + PMD_SIZE) & PMD_MASK;
			if (next_pmd == 0)
				next_pmd = vend;

			m_pmd = hydra_walk_to_pmd(mm, pmd_addr, master_node);
			if (HYDRA_WALK_BAD(m_pmd))
				continue;
			m_pmdval = READ_ONCE(*m_pmd);

			for (node = 0; node < NUMA_NODE_COUNT; node++) {
				pmd_t *r_pmd;
				pmd_t r_pmdval;

				if (node == master_node || !mm->repl_pgd[node])
					continue;

				r_pmd = hydra_walk_to_pmd(mm, pmd_addr, node);
				if (HYDRA_WALK_BAD(r_pmd))
					continue;
				r_pmdval = READ_ONCE(*r_pmd);

				if ((pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval)) &&
				    (pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval))) {
					if (!pmd_write(m_pmdval) && pmd_write(r_pmdval)) {
						hdbg_printf(st, "CHECK_FAIL\tTYPE=WRPROTECT\tLEVEL=PMD_HUGE\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=MASTER_RO_REPLICA_RW\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\n",
							    pmd_addr, node, master_node,
							    pmd_val(m_pmdval), pmd_val(r_pmdval));
						(*fail_count)++;
					}
					continue;
				}

				if (pmd_none(m_pmdval) || pmd_bad(m_pmdval) ||
				    pmd_trans_huge(m_pmdval) || pmd_leaf(m_pmdval))
					continue;
				if (pmd_none(r_pmdval) || pmd_bad(r_pmdval) ||
				    pmd_trans_huge(r_pmdval) || pmd_leaf(r_pmdval))
					continue;

				{
					pte_t *m_base = pte_offset_kernel(m_pmd, pmd_addr & PMD_MASK);
					pte_t *r_base = pte_offset_kernel(r_pmd, pmd_addr & PMD_MASK);
					int i;
					int start_i = 0, end_i = PTRS_PER_PTE;

					if ((pmd_addr & PMD_MASK) < vstart)
						start_i = (vstart - (pmd_addr & PMD_MASK)) >> PAGE_SHIFT;
					if ((pmd_addr & PMD_MASK) + PMD_SIZE > vend)
						end_i = (vend - (pmd_addr & PMD_MASK)) >> PAGE_SHIFT;

					for (i = start_i; i < end_i; i++) {
						pte_t mv = READ_ONCE(m_base[i]);
						pte_t rv = READ_ONCE(r_base[i]);
						unsigned long page_va;

						if (!pte_present(mv) || !pte_present(rv))
							continue;
						if (pte_pfn(mv) != pte_pfn(rv))
							continue;

						page_va = (pmd_addr & PMD_MASK) + ((unsigned long)i << PAGE_SHIFT);

						if (!pte_write(mv) && pte_write(rv)) {
							hdbg_printf(st, "CHECK_FAIL\tTYPE=WRPROTECT\tLEVEL=PTE\tVA=0x%lx\tNODE=%d\tMASTER_NODE=%d\tREASON=MASTER_RO_REPLICA_RW\tM_ENTRY=0x%lx\tR_ENTRY=0x%lx\n",
								    page_va, node, master_node,
								    pte_val(mv), pte_val(rv));
							(*fail_count)++;
						}
					}
				}
			}
		}
	}
}

static void hdbg_tree_structure(struct hydra_debug_state *st,
				struct mm_struct *mm,
				unsigned long range_start,
				unsigned long range_end)
{
	int node;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, range_start);
		unsigned long prev_pud_va = ~0UL;

		if (!mm->repl_pgd[node])
			continue;

		hdbg_printf(st, "TREE_NODE\tNODE=%d\tPGD=%px\tPGD_PA=0x%lx\tPGD_NID=%d\n",
			    node, mm->repl_pgd[node],
			    virt_addr_valid(mm->repl_pgd[node]) ? __pa(mm->repl_pgd[node]) : 0,
			    page_nid_safe(mm->repl_pgd[node]));

		for_each_vma(vmi, vma) {
			unsigned long addr, next;
			unsigned long vstart, vend;

			if (vma->vm_start >= range_end)
				break;

			vstart = max(vma->vm_start, range_start);
			vend = min(vma->vm_end, range_end);

			for (addr = vstart & PUD_MASK; addr < vend; addr = next) {
				pgd_t *pgd;
				p4d_t *p4d;
				pud_t *pud;
				unsigned long pmd_addr, next_pmd;
				int pmd_count = 0, pmd_huge = 0, pmd_none_c = 0;
				int pte_tables = 0;

				next = (addr + PUD_SIZE) & PUD_MASK;
				if (next == 0 || next > vend)
					next = vend;

				if (addr == prev_pud_va)
					continue;
				prev_pud_va = addr;

				pgd = pgd_offset_pgd(mm->repl_pgd[node], addr);
				if (!pgd_present(*pgd))
					continue;
				p4d = p4d_offset(pgd, addr);
				if (p4d_none(*p4d) || p4d_bad(*p4d))
					continue;
				pud = pud_offset(p4d, addr);

				if (pud_none(*pud)) {
					hdbg_printf(st, "TREE_PUD\tNODE=%d\tVA=0x%lx\tSTATUS=NONE\n",
						    node, addr);
					continue;
				}

				if (pud_leaf(*pud)) {
					hdbg_printf(st, "TREE_PUD\tNODE=%d\tVA=0x%lx\tSTATUS=HUGE\tPFN=0x%lx\n",
						    node, addr, pud_pfn(*pud));
					continue;
				}

				if (pud_bad(*pud)) {
					hdbg_printf(st, "TREE_PUD\tNODE=%d\tVA=0x%lx\tSTATUS=BAD\n",
						    node, addr);
					continue;
				}

				for (pmd_addr = addr; pmd_addr < (addr + PUD_SIZE) && pmd_addr < vend;
				     pmd_addr = next_pmd) {
					pmd_t *pmd;
					next_pmd = (pmd_addr + PMD_SIZE) & PMD_MASK;
					if (next_pmd == 0)
						break;

					pmd = pmd_offset(pud, pmd_addr);
					if (pmd_none(*pmd)) {
						pmd_none_c++;
					} else if (pmd_trans_huge(*pmd) || pmd_leaf(*pmd)) {
						pmd_huge++;
						pmd_count++;
					} else if (!pmd_bad(*pmd)) {
						pte_tables++;
						pmd_count++;
					} else {
						pmd_count++;
					}
				}

				{
					pmd_t *pmd_base = pmd_offset(pud, addr);
					struct page *pmd_page = virt_addr_valid(pmd_base) ? virt_to_page(pmd_base) : NULL;

					hdbg_printf(st, "TREE_PUD\tNODE=%d\tVA=0x%lx\tSTATUS=TABLE\tPMD_TBL=%px\tPMD_TBL_PA=0x%lx\tPMD_TBL_NID=%d\tPMD_POPULATED=%d\tPMD_NONE=%d\tPMD_HUGE=%d\tPTE_TABLES=%d\tCHAIN_LEN=%d\tCIRCULAR=%d\n",
						    node, addr,
						    pmd_base,
						    virt_addr_valid(pmd_base) ? __pa(pmd_base) : 0,
						    page_nid_safe(pmd_base),
						    pmd_count, pmd_none_c, pmd_huge, pte_tables,
						    pmd_page ? chain_length(pmd_page) : 0,
						    pmd_page ? chain_is_circular(pmd_page) : 0);
				}
			}
		}
	}
}

typedef void (*hydra_cmd_fn)(struct hydra_debug_state *st,
			     struct mm_struct *mm,
			     unsigned long start, unsigned long end,
			     int has_range);

static void cmd_translate(struct hydra_debug_state *st,
			  struct mm_struct *mm,
			  unsigned long start, unsigned long end,
			  int has_range)
{
	if (!has_range) {
		hdbg_printf(st, "ERROR\tMSG=translate requires: pid va_hex\n");
		return;
	}
	mmap_read_lock(mm);
	hdbg_translate(st, mm, start);
	mmap_read_unlock(mm);
}

static void cmd_walk(struct hydra_debug_state *st,
		     struct mm_struct *mm,
		     unsigned long start, unsigned long end,
		     int has_range)
{
	mmap_read_lock(mm);
	hdbg_walk(st, mm, start, has_range ? end : TASK_SIZE, has_range);
	mmap_read_unlock(mm);
}

static void cmd_chains(struct hydra_debug_state *st,
		       struct mm_struct *mm,
		       unsigned long start, unsigned long end,
		       int has_range)
{
	mmap_read_lock(mm);
	hdbg_chains(st, mm, has_range ? start : 0,
		    has_range ? end : TASK_SIZE, has_range);
	mmap_read_unlock(mm);
}

static void cmd_check(struct hydra_debug_state *st,
		      struct mm_struct *mm,
		      unsigned long start, unsigned long end,
		      int has_range)
{
	int fails = 0;
	unsigned long rs = has_range ? start : 0;
	unsigned long re = has_range ? end : TASK_SIZE;

	mmap_read_lock(mm);

	hdbg_printf(st, "CHECK_BEGIN\tREPL_ENABLED=%d\tRANGE=0x%lx-0x%lx\n",
		    mm->lazy_repl_enabled ? 1 : 0, rs, re);

	hdbg_printf(st, "CHECK_PHASE\tNAME=VMA_MASTER_NODE\n");
	hdbg_check_vma_master_node(st, mm, &fails);

	hdbg_printf(st, "CHECK_PHASE\tNAME=PGD_KERNEL_HALF\n");
	hdbg_check_pgd_kernel(st, mm, &fails);

	if (mm->lazy_repl_enabled) {
		hdbg_printf(st, "CHECK_PHASE\tNAME=MASTER_INVARIANT\n");
		hdbg_check_master_invariant(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=CHAIN_INTEGRITY\n");
		hdbg_check_chain_integrity(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=NODE_LOCALITY\n");
		hdbg_check_node_locality(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=CHAIN_SUBSET\n");
		hdbg_check_pte_subset(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=ORPHAN_REPLICAS\n");
		hdbg_check_cross_node_reachability(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=STALE_UNMAPPED\n");
		hdbg_check_stale_replicas(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=OWNER_MM\n");
		hdbg_check_owner_mm(st, mm, rs, re, &fails);

		hdbg_printf(st, "CHECK_PHASE\tNAME=WRPROTECT_CONSISTENCY\n");
		hdbg_check_wrprotect_consistency(st, mm, rs, re, &fails);
	}

	hdbg_printf(st, "CHECK_PHASE\tNAME=DEPOSITS\n");
	hdbg_check_deposits(st, mm, rs, re, &fails);

	hdbg_printf(st, "CHECK_END\tTOTAL_FAILS=%d\tRESULT=%s\n",
		    fails, fails == 0 ? "PASS" : "FAIL");

	mmap_read_unlock(mm);
}

static void cmd_deposits(struct hydra_debug_state *st,
			 struct mm_struct *mm,
			 unsigned long start, unsigned long end,
			 int has_range)
{
	int dummy = 0;

	mmap_read_lock(mm);
	hdbg_check_deposits(st, mm, has_range ? start : 0,
			    has_range ? end : TASK_SIZE, &dummy);
	mmap_read_unlock(mm);
}

static void cmd_summary(struct hydra_debug_state *st,
			struct mm_struct *mm,
			unsigned long start, unsigned long end,
			int has_range)
{
	mmap_read_lock(mm);
	hdbg_summary(st, mm);
	mmap_read_unlock(mm);
}

static void cmd_pte_dump(struct hydra_debug_state *st,
			 struct mm_struct *mm,
			 unsigned long start, unsigned long end,
			 int has_range)
{
	if (!has_range) {
		hdbg_printf(st, "ERROR\tMSG=pte_dump requires: pid start_hex end_hex\n");
		return;
	}
	mmap_read_lock(mm);
	hdbg_pte_dump(st, mm, start, end);
	mmap_read_unlock(mm);
}

static void cmd_cross_compare(struct hydra_debug_state *st,
			      struct mm_struct *mm,
			      unsigned long start, unsigned long end,
			      int has_range)
{
	mmap_read_lock(mm);
	hdbg_cross_compare(st, mm, has_range ? start : 0,
			   has_range ? end : TASK_SIZE);
	mmap_read_unlock(mm);
}

static void cmd_owner_check(struct hydra_debug_state *st,
			    struct mm_struct *mm,
			    unsigned long start, unsigned long end,
			    int has_range)
{
	int fails = 0;

	mmap_read_lock(mm);
	hdbg_check_owner_mm(st, mm, has_range ? start : 0,
			    has_range ? end : TASK_SIZE, &fails);
	hdbg_printf(st, "OWNER_CHECK_END\tFAILS=%d\tRESULT=%s\n",
		    fails, fails == 0 ? "PASS" : "FAIL");
	mmap_read_unlock(mm);
}

static void cmd_wrprotect(struct hydra_debug_state *st,
			  struct mm_struct *mm,
			  unsigned long start, unsigned long end,
			  int has_range)
{
	int fails = 0;

	mmap_read_lock(mm);
	hdbg_check_wrprotect_consistency(st, mm, has_range ? start : 0,
					 has_range ? end : TASK_SIZE, &fails);
	hdbg_printf(st, "WRPROTECT_CHECK_END\tFAILS=%d\tRESULT=%s\n",
		    fails, fails == 0 ? "PASS" : "FAIL");
	mmap_read_unlock(mm);
}

static void cmd_tree(struct hydra_debug_state *st,
		     struct mm_struct *mm,
		     unsigned long start, unsigned long end,
		     int has_range)
{
	mmap_read_lock(mm);
	hdbg_tree_structure(st, mm, has_range ? start : 0,
			    has_range ? end : TASK_SIZE);
	mmap_read_unlock(mm);
}

struct hydra_debug_file {
	struct hydra_debug_state state;
	hydra_cmd_fn fn;
};

static int hydra_debug_open(struct inode *inode, struct file *file)
{
	struct hydra_debug_file *df;

	df = kzalloc(sizeof(*df), GFP_KERNEL);
	if (!df)
		return -ENOMEM;

	mutex_init(&df->state.lock);
	df->fn = pde_data(inode);
	file->private_data = df;
	return 0;
}

static ssize_t hydra_debug_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct hydra_debug_file *df = file->private_data;
	struct hydra_debug_state *st = &df->state;
	struct mm_struct *mm;
	char buf[128];
	size_t len;
	pid_t pid;
	unsigned long start = 0, end = 0;
	int n, has_range = 0;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	n = sscanf(buf, "%d %lx %lx", &pid, &start, &end);
	if (n < 1)
		return -EINVAL;
	if (n >= 2 && n < 3)
		end = start + PAGE_SIZE;
	if (n >= 2)
		has_range = 1;

	mm = hydra_debug_get_mm(pid);
	if (!mm)
		return -ESRCH;

	mutex_lock(&st->lock);

	if (st->buf) {
		kvfree(st->buf);
		st->buf = NULL;
	}
	st->len = 0;
	st->cap = 0;

	df->fn(st, mm, start, end, has_range);

	mutex_unlock(&st->lock);

	mmput(mm);
	return count;
}

static ssize_t hydra_debug_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct hydra_debug_file *df = file->private_data;
	struct hydra_debug_state *st = &df->state;
	ssize_t ret;

	mutex_lock(&st->lock);

	if (!st->buf || *ppos >= st->len) {
		mutex_unlock(&st->lock);
		return 0;
	}

	ret = min(count, st->len - (size_t)*ppos);
	if (copy_to_user(ubuf, st->buf + *ppos, ret)) {
		mutex_unlock(&st->lock);
		return -EFAULT;
	}

	*ppos += ret;
	mutex_unlock(&st->lock);
	return ret;
}

static int hydra_debug_release(struct inode *inode, struct file *file)
{
	struct hydra_debug_file *df = file->private_data;

	if (df->state.buf)
		kvfree(df->state.buf);
	kfree(df);
	return 0;
}

static const struct proc_ops hydra_debug_ops = {
	.proc_open	= hydra_debug_open,
	.proc_write	= hydra_debug_write,
	.proc_read	= hydra_debug_read,
	.proc_release	= hydra_debug_release,
	.proc_lseek	= default_llseek,
};

static struct proc_dir_entry *hydra_debug_dir;

static int __init hydra_debug_init(void)
{
	struct proc_dir_entry *parent;

	parent = proc_mkdir("hydra/debug", NULL);
	if (!parent)
		return -ENOMEM;

	hydra_debug_dir = parent;

	if (!proc_create_data("translate", 0666, parent,
			      &hydra_debug_ops, cmd_translate))
		goto fail;

	if (!proc_create_data("walk", 0666, parent,
			      &hydra_debug_ops, cmd_walk))
		goto fail;

	if (!proc_create_data("chains", 0666, parent,
			      &hydra_debug_ops, cmd_chains))
		goto fail;

	if (!proc_create_data("check", 0666, parent,
			      &hydra_debug_ops, cmd_check))
		goto fail;

	if (!proc_create_data("deposits", 0666, parent,
			      &hydra_debug_ops, cmd_deposits))
		goto fail;

	if (!proc_create_data("summary", 0666, parent,
			      &hydra_debug_ops, cmd_summary))
		goto fail;

	if (!proc_create_data("pte_dump", 0666, parent,
			      &hydra_debug_ops, cmd_pte_dump))
		goto fail;

	if (!proc_create_data("cross_compare", 0666, parent,
			      &hydra_debug_ops, cmd_cross_compare))
		goto fail;

	if (!proc_create_data("owner_check", 0666, parent,
			      &hydra_debug_ops, cmd_owner_check))
		goto fail;

	if (!proc_create_data("wrprotect", 0666, parent,
			      &hydra_debug_ops, cmd_wrprotect))
		goto fail;

	if (!proc_create_data("tree", 0666, parent,
			      &hydra_debug_ops, cmd_tree))
		goto fail;

	pr_info("HYDRA: debug procfs interfaces registered under /proc/hydra/debug/\n");
	return 0;

fail:
	remove_proc_subtree("hydra/debug", NULL);
	return -ENOMEM;
}
late_initcall(hydra_debug_init);
