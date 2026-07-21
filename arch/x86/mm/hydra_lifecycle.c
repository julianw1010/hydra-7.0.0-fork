#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/cpumask.h>
#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/hydra.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>

int hydra_nr_sockets;
int hydra_node_socket[NUMA_NODE_COUNT];
int hydra_socket_rep[NUMA_NODE_COUNT];
nodemask_t hydra_socket_nodes[NUMA_NODE_COUNT];
cpumask_t hydra_socket_cpumask[NUMA_NODE_COUNT];

static int __init hydra_topology_init(void)
{
	int pkg_of[NUMA_NODE_COUNT];
	int seen_pkg[NUMA_NODE_COUNT];
	int node, other, cpu, s;

	for (node = 0; node < NUMA_NODE_COUNT; node++) {
		pkg_of[node] = -1;
		hydra_node_socket[node] = -1;
		hydra_socket_rep[node] = -1;
	}

	for_each_online_node(node) {
		BUG_ON(node >= NUMA_NODE_COUNT);
		for_each_cpu(cpu, cpumask_of_node(node)) {
			pkg_of[node] = topology_physical_package_id(cpu);
			break;
		}
	}

	for_each_online_node(node) {
		int best = -1, best_dist = INT_MAX;

		if (pkg_of[node] >= 0)
			continue;
		for_each_online_node(other) {
			if (pkg_of[other] < 0)
				continue;
			if (node_distance(node, other) < best_dist) {
				best_dist = node_distance(node, other);
				best = other;
			}
		}
		if (best < 0) {
			pr_emerg("HYDRA: topology: no socket resolvable for node %d\n",
				 node);
			BUG();
		}
		pkg_of[node] = pkg_of[best];
	}

	hydra_nr_sockets = 0;
	for_each_online_node(node) {
		for (s = 0; s < hydra_nr_sockets; s++) {
			if (seen_pkg[s] == pkg_of[node])
				break;
		}
		if (s == hydra_nr_sockets) {
			seen_pkg[s] = pkg_of[node];
			hydra_socket_rep[s] = node;
			hydra_nr_sockets++;
		}
		hydra_node_socket[node] = s;
		node_set(node, hydra_socket_nodes[s]);
	}

	for (s = 0; s < hydra_nr_sockets; s++) {
		cpumask_clear(&hydra_socket_cpumask[s]);
		for_each_node_mask(node, hydra_socket_nodes[s])
			cpumask_or(&hydra_socket_cpumask[s],
				   &hydra_socket_cpumask[s],
				   cpumask_of_node(node));
	}

	pr_info("HYDRA: topology: %d socket(s)\n", hydra_nr_sockets);
	for (s = 0; s < hydra_nr_sockets; s++)
		pr_info("HYDRA: socket %d: rep node %d nodes %*pbl\n",
			s, hydra_socket_rep[s],
			nodemask_pr_args(&hydra_socket_nodes[s]));

	return 0;
}
late_initcall(hydra_topology_init);

void hydra_reload_cr3(void *info)
{
	struct mm_struct *mm = info;
	if (this_cpu_read(cpu_tlbstate.loaded_mm) == mm) {
		int node = numa_node_id();
		unsigned long cur_cr3 = __read_cr3();
		unsigned long new_pa = __pa(mm->repl_pgd[node]);

		if ((cur_cr3 & PAGE_MASK) != new_pa) {
			native_write_cr3(new_pa | (cur_cr3 & ~PAGE_MASK));
			__flush_tlb_all();
		}
	}
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

	WRITE_ONCE(mm->lazy_repl_enabled, true);
	smp_mb();
	on_each_cpu_mask(mm_cpumask(mm), hydra_reload_cr3, mm, 1);
	flush_tlb_mm(mm);

	{
		int count = 0;
		for (i = 0; i < NUMA_NODE_COUNT; i++) {
			if (hydra_repl_pgd_first(mm, i))
				count++;
		}
		count++;
		printk(KERN_INFO "HYDRA: enabled page table replication for mm %px on %d trees\n", mm, count);
	}

	mmap_write_unlock(mm);

	return 0;
}

SYSCALL_DEFINE0(set_pgtblreplpolicy)
{
	return hydra_enable_replication(current->mm);
}
