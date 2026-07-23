#include <linux/mm.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/seq_file.h>
#include <linux/hydra.h>

int hydra_nr_domains;
int hydra_domain_dist;
static bool hydra_domain_dist_auto = true;
static int hydra_node_domain_map[NUMA_NODE_COUNT];
static int hydra_domain_home_map[NUMA_NODE_COUNT];
nodemask_t hydra_domain_nodemask[NUMA_NODE_COUNT];

int hydra_node_domain(int node)
{
	if (node < 0 || node >= NUMA_NODE_COUNT || !hydra_nr_domains)
		return node;
	return hydra_node_domain_map[node];
}

int hydra_domain_home(int domain)
{
	if (domain < 0 || domain >= hydra_nr_domains)
		return domain;
	return hydra_domain_home_map[domain];
}

int hydra_node_home(int node)
{
	int d = hydra_node_domain(node);

	if (d < 0 || d >= hydra_nr_domains)
		return node;
	return hydra_domain_home_map[d];
}

bool hydra_same_domain(int a, int b)
{
	return hydra_node_domain(a) == hydra_node_domain(b);
}

static int __init hydra_domain_dist_setup(char *str)
{
	int val;

	if (!kstrtoint(str, 10, &val) && val > 0) {
		hydra_domain_dist = val;
		hydra_domain_dist_auto = false;
	}
	return 1;
}
__setup("hydra_domain_dist=", hydra_domain_dist_setup);

static int __init hydra_domain_root(int *parent, int node)
{
	while (parent[node] != node) {
		parent[node] = parent[parent[node]];
		node = parent[node];
	}
	return node;
}

static int __init hydra_domain_init(void)
{
	int parent[NUMA_NODE_COUNT];
	int domain_of_root[NUMA_NODE_COUNT];
	int i, j, d;

	if (!hydra_domain_dist)
		hydra_domain_dist = REMOTE_DISTANCE;

	for_each_online_node(i) {
		if (i >= NUMA_NODE_COUNT) {
			pr_emerg("HYDRA: online node %d exceeds CONFIG_HYDRA_NUMA_NODE_COUNT=%d\n",
				 i, NUMA_NODE_COUNT);
			BUG();
		}
	}

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		parent[i] = i;
		domain_of_root[i] = -1;
		hydra_node_domain_map[i] = NUMA_NODE_COUNT + i;
		nodes_clear(hydra_domain_nodemask[i]);
	}

	for_each_online_node(i) {
		for_each_online_node(j) {
			if (i == j ||
			    node_distance(i, j) >= hydra_domain_dist)
				continue;
			parent[hydra_domain_root(parent, i)] =
				hydra_domain_root(parent, j);
		}
	}

	for_each_online_node(i) {
		int root = hydra_domain_root(parent, i);

		if (domain_of_root[root] < 0)
			domain_of_root[root] = hydra_nr_domains++;
		hydra_node_domain_map[i] = domain_of_root[root];
		node_set(i, hydra_domain_nodemask[domain_of_root[root]]);
	}

	for (d = 0; d < hydra_nr_domains; d++) {
		int best = -1;

		for_each_node_mask(i, hydra_domain_nodemask[d]) {
			if (best < 0 ||
			    node_present_pages(i) > node_present_pages(best))
				best = i;
		}
		hydra_domain_home_map[d] = best;
	}

	pr_info("HYDRA: %d replication domains over %d nodes (distance threshold %d%s)\n",
		hydra_nr_domains, num_online_nodes(), hydra_domain_dist,
		hydra_domain_dist_auto ? ", auto" : "");
	for (d = 0; d < hydra_nr_domains; d++)
		pr_info("HYDRA: domain %d: home n%d, nodes %*pbl\n",
			d, hydra_domain_home_map[d],
			nodemask_pr_args(&hydra_domain_nodemask[d]));

	return 0;
}
early_initcall(hydra_domain_init);

int hydra_domains_proc_show(struct seq_file *m, void *v)
{
	int d;

	seq_puts(m, " Hydra replication domains\n");
	seq_printf(m, " %-20s %d%s\n", "distance threshold",
		   hydra_domain_dist, hydra_domain_dist_auto ? " (auto)" : "");
	seq_printf(m, " %-20s %d\n", "domains", hydra_nr_domains);
	seq_puts(m, " rows = domain: home node + member nodes\n");
	seq_puts(m, " --------------------------------------------------\n");
	for (d = 0; d < hydra_nr_domains; d++)
		seq_printf(m, " domain %-3d  home n%-3d  nodes %*pbl\n",
			   d, hydra_domain_home_map[d],
			   nodemask_pr_args(&hydra_domain_nodemask[d]));
	return 0;
}
