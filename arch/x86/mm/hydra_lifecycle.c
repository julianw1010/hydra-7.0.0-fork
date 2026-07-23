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
		int node = hydra_effective_node(mm);
		unsigned long cur_cr3 = __read_cr3();
		unsigned long new_pa = __pa(mm->repl_pgd[node]);

		if ((cur_cr3 & PAGE_MASK) != new_pa) {
			native_write_cr3(new_pa | (cur_cr3 & ~PAGE_MASK));
			__flush_tlb_all();
		}
	}
}

void hydra_force_steering_switch(struct mm_struct *mm)
{
	if (!mm || !READ_ONCE(mm->lazy_repl_enabled))
		return;

	on_each_cpu_mask(mm_cpumask(mm), hydra_reload_cr3, mm, 1);
}

static void hydra_prebuild_interior(struct mm_struct *mm, int primary)
{
	unsigned long addr, end = mm->task_size;
	unsigned long next_pgd, next_p4d, next_pud;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	long built = 0;
	int j;

	if (!READ_ONCE(sysctl_hydra_prebuild) || !end)
		return;

	addr = 0;
	pgd = pgd_offset_pgd(mm->pgd, addr);
	do {
		next_pgd = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			goto next_pgd_pb;
		p4d = p4d_offset(pgd, addr);
		do {
			next_p4d = p4d_addr_end(addr, next_pgd);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				goto next_p4d_pb;
			pud = pud_offset(p4d, addr);
			do {
				next_pud = pud_addr_end(addr, next_p4d);
				if (pud_none(*pud) || pud_trans_huge(*pud) ||
				    pud_bad(*pud))
					goto next_pud_pb;

				for (j = 0; j < NUMA_NODE_COUNT; j++) {
					pgd_t *rpgd;
					p4d_t *rp4d;
					pud_t *rpud;

					if (j == primary || !mm->repl_pgd[j])
						continue;
					rpgd = pgd_offset_pgd(mm->repl_pgd[j], addr);
					rp4d = p4d_alloc(mm, rpgd, addr);
					if (!rp4d)
						continue;
					rpud = pud_alloc(mm, rp4d, addr);
					if (!rpud)
						continue;
					if (pud_none(*rpud) &&
					    !hydra_alloc_replica_pmd(mm, rpud, addr, primary))
						built++;
				}
next_pud_pb:
				addr = next_pud;
			} while (pud++, addr != next_p4d);
next_p4d_pb:
			addr = next_p4d;
		} while (p4d++, addr != next_pgd);
next_pgd_pb:
		addr = next_pgd;
	} while (pgd++, addr != end);

	if (built && mm->hydra_stats)
		atomic_long_add(built, &mm->hydra_stats->coh_prebuilt);
}

int hydra_enable_replication(struct mm_struct *mm)
{
	int i, primary_node;

	if (mm->lazy_repl_enabled)
		return 0;

	static_branch_enable(&hydra_repl_ever_enabled);

	mmap_write_lock(mm);

	if (mm->lazy_repl_enabled) {
		mmap_write_unlock(mm);
		return 0;
	}

	primary_node = page_to_nid(virt_to_page(mm->pgd));

	set_bit(primary_node, &mm->hydra_active_nodes);
	if (numa_node_id() >= 0 && numa_node_id() < NUMA_NODE_COUNT)
		set_bit(numa_node_id(), &mm->hydra_active_nodes);

	{
		struct xarray *xa = kzalloc(sizeof(*xa), GFP_KERNEL);

		if (!xa) {
			pr_emerg("HYDRA: pud owner map allocation failed for mm %px during enable\n",
				 mm);
			BUG();
		}
		xa_init(xa);
		mm->hydra_pud_owner = xa;
	}

	{
		struct vm_area_struct *vma;
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
			hydra_vma_chown(vma, primary_node);
		}
	}

	hydra_stats_mark_enabled(mm, primary_node);

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (i == primary_node) {
			mm->repl_pgd[i] = mm->pgd;
		} else {
			mm->repl_pgd[i] = hydra_repl_pgd_alloc(mm, i);
			if (!mm->repl_pgd[i]) {
				pr_emerg("HYDRA: replica PGD allocation failed for mm %px node %d during enable\n",
					 mm, i);
				BUG();
			}
		}
	}

	hydra_prebuild_interior(mm, primary_node);

	WRITE_ONCE(mm->lazy_repl_enabled, true);
	smp_mb();
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

	mmap_write_unlock(mm);

	return 0;
}

SYSCALL_DEFINE0(set_pgtblreplpolicy)
{
	return hydra_enable_replication(current->mm);
}
