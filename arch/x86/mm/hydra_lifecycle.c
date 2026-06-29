#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>

void hydra_reload_cr3(void *info)
{
	struct mm_struct *mm = info;
	if (this_cpu_read(cpu_tlbstate.loaded_mm) == mm) {
		int node = numa_node_id();
		unsigned long new_cr3 = __pa(mm->repl_pgd[node]);
		write_cr3(new_cr3);
	}
}

int hydra_enable_replication(struct mm_struct *mm)
{
	int i, primary_node;

	if (mm->lazy_repl_enabled)
		return 0;

	mmap_write_lock(mm);

	if (mm->lazy_repl_enabled) {
		mmap_write_unlock(mm);
		return 0;
	}

	primary_node = page_to_nid(virt_to_page(mm->pgd));

	{
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			WRITE_ONCE(vma->master_pgd_node, primary_node);
		}
	}

	hydra_stats_attach(mm, primary_node);
	hydra_vma_owner_seed(mm, primary_node);

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (i == primary_node) {
			mm->repl_pgd[i] = mm->pgd;
		} else {
			mm->repl_pgd[i] = hydra_repl_pgd_alloc(mm, i);
			if (!mm->repl_pgd[i])
				mm->repl_pgd[i] = mm->pgd;
		}
	}

	WRITE_ONCE(mm->lazy_repl_enabled, true);
	smp_wmb();
	on_each_cpu_mask(mm_cpumask(mm), hydra_reload_cr3, mm, 1);
	flush_tlb_mm(mm);

	{
		int count = 0;
		for (i = 0; i < NUMA_NODE_COUNT; i++) {
			if (mm->repl_pgd[i] && mm->repl_pgd[i] != mm->pgd)
				count++;
		}
		count++;
		printk(KERN_INFO "HYDRA: enabled page table replication for mm %px on %d nodes\n", mm, count);
	}

	hydra_stats_seed(mm);

	mmap_write_unlock(mm);

	hydra_map_ldt_to_replicas(mm);

	return 0;
}

SYSCALL_DEFINE0(set_pgtblreplpolicy)
{
	return hydra_enable_replication(current->mm);
}
