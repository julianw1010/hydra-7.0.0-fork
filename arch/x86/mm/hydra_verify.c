#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/processor.h>

int sysctl_hydra_verify __read_mostly;

#define HYDRA_VERIFY_RECHECK 1000000

static void hydra_verify_check_child(struct mm_struct *mm, int node,
				     const char *level, unsigned long entry_val)
{
	unsigned long phys = entry_val & PTE_PFN_MASK;
	struct page *child;
	int child_node;

	if (!phys)
		return;

	child = pfn_to_page(phys >> PAGE_SHIFT);
	child_node = page_to_nid(child);
	if (child_node != node) {
		pr_emerg("HYDRA verify: mm %px node %d %s entry points to table on node %d (cross-node)\n",
			 mm, node, level, child_node);
		BUG();
	}
}

static bool hydra_walk_addr_check(struct mm_struct *mm, unsigned long address,
				  int node, pgd_t *pgd, bool check_locality)
{
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	pgd_t pgdv;
	p4d_t p4dv;
	pud_t pudv;
	pmd_t pmdv;

	pgdp = pgd_offset_pgd(pgd, address);
	pgdv = READ_ONCE(*pgdp);
	if (pgd_none(pgdv) || !pgd_present(pgdv))
		return false;
	if (check_locality && pgtable_l5_enabled())
		hydra_verify_check_child(mm, node, "PGD", pgd_val(pgdv));

	p4dp = p4d_offset(pgdp, address);
	p4dv = READ_ONCE(*p4dp);
	if (p4d_none(p4dv) || !p4d_present(p4dv))
		return false;
	if (check_locality)
		hydra_verify_check_child(mm, node, "P4D", p4d_val(p4dv));

	pudp = pud_offset(p4dp, address);
	pudv = READ_ONCE(*pudp);
	if (pud_none(pudv) || !pud_present(pudv))
		return false;
	if (pud_leaf(pudv))
		return true;
	if (check_locality)
		hydra_verify_check_child(mm, node, "PUD", pud_val(pudv));

	pmdp = pmd_offset(pudp, address);
	pmdv = READ_ONCE(*pmdp);
	if (pmd_none(pmdv) || !pmd_present(pmdv))
		return false;
	if (pmd_trans_huge(pmdv) || pmd_leaf(pmdv))
		return true;
	if (pmd_bad(pmdv))
		return false;
	if (check_locality)
		hydra_verify_check_child(mm, node, "PTE", pmd_val(pmdv));

	ptep = pte_offset_kernel(pmdp, address);
	return pte_present(READ_ONCE(*ptep));
}

void hydra_verify_fault_addr(struct mm_struct *mm, unsigned long address)
{
	int fault_node = numa_node_id();
	struct vm_area_struct *vma;
	unsigned long cr3_pa;
	pgd_t *loaded_pgd, *expected_pgd, *mpgd;
	int cr3_node, master_node;
	int i;

	if (!mm || !READ_ONCE(mm->lazy_repl_enabled))
		return;
	if (fault_node < 0 || fault_node >= NUMA_NODE_COUNT)
		return;

	expected_pgd = READ_ONCE(mm->repl_pgd[fault_node]);
	if (!expected_pgd)
		return;

	cr3_pa = read_cr3_pa();
	loaded_pgd = (pgd_t *)__va(cr3_pa);
	cr3_node = page_to_nid(virt_to_page(loaded_pgd));

	if (loaded_pgd != expected_pgd || cr3_node != fault_node) {
		pr_emerg("HYDRA verify: mm %px cpu %d node %d runs CR3 pgd %px (node %d) but repl_pgd[%d] is %px\n",
			 mm, smp_processor_id(), fault_node, loaded_pgd, cr3_node,
			 fault_node, expected_pgd);
		BUG();
	}

	rcu_read_lock();

	if (!hydra_walk_addr_check(mm, address, cr3_node, loaded_pgd, true))
		goto out;

	vma = vma_lookup(mm, address);
	if (!vma)
		goto out;

	master_node = (int)READ_ONCE(vma->master_pgd_node);
	if (master_node < 0 || master_node >= NUMA_NODE_COUNT ||
	    master_node == cr3_node)
		goto out;

	mpgd = READ_ONCE(mm->repl_pgd[master_node]);
	if (!mpgd)
		goto out;

	for (i = 0; i < HYDRA_VERIFY_RECHECK; i++) {
		if (hydra_walk_addr_check(mm, address, master_node, mpgd, false))
			goto out;
		if (!hydra_walk_addr_check(mm, address, cr3_node, loaded_pgd, false))
			goto out;
		cpu_relax();
	}

	pr_emerg("HYDRA verify: mm %px faulted entry at %lx present on node %d but absent on master node %d\n",
		 mm, address, cr3_node, master_node);
	BUG();

out:
	rcu_read_unlock();
}
