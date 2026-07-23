#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/topology.h>
#include <linux/nodemask.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/hydra.h>

struct hydra_topology hydra_topo;

int sysctl_hydra_domain_dist;

static int hydra_domain_root(int *parent, int n)
{
	while (parent[n] != n)
		n = parent[n];
	return n;
}

static int hydra_auto_threshold(void)
{
	int vals[NUMA_NODE_COUNT * NUMA_NODE_COUNT];
	int nvals = 0;
	int i, j, k, best_gap, thr;

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		for (j = 0; j < NUMA_NODE_COUNT; j++) {
			int d;
			bool seen = false;

			if (i == j)
				continue;
			d = hydra_topo.dist[i][j];
			for (k = 0; k < nvals; k++) {
				if (vals[k] == d) {
					seen = true;
					break;
				}
			}
			if (!seen)
				vals[nvals++] = d;
		}
	}

	if (!nvals)
		return LOCAL_DISTANCE;

	for (i = 0; i < nvals; i++) {
		for (j = i + 1; j < nvals; j++) {
			if (vals[j] < vals[i])
				swap(vals[i], vals[j]);
		}
	}

	if (nvals == 1)
		return LOCAL_DISTANCE;

	best_gap = 0;
	thr = LOCAL_DISTANCE;
	for (i = 0; i < nvals - 1; i++) {
		int gap = vals[i + 1] - vals[i];

		if (gap >= best_gap) {
			best_gap = gap;
			thr = vals[i];
		}
	}

	return thr;
}

void hydra_topology_update(void)
{
	int parent[NUMA_NODE_COUNT];
	int i, j, thr, ndom;

	BUILD_BUG_ON(NUMA_NODE_COUNT > 32);

	hydra_topo.nr_nodes = NUMA_NODE_COUNT;
	hydra_topo.min_offnode_dist = 255;

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		for (j = 0; j < NUMA_NODE_COUNT; j++) {
			int d = node_distance(i, j);

			if (d < 0 || d > 255)
				d = 255;
			hydra_topo.dist[i][j] = d;
			if (i != j && d < hydra_topo.min_offnode_dist)
				hydra_topo.min_offnode_dist = d;
		}
	}

	thr = READ_ONCE(sysctl_hydra_domain_dist);
	if (thr <= 0)
		thr = hydra_auto_threshold();
	hydra_topo.share_dist = thr;

	for (i = 0; i < NUMA_NODE_COUNT; i++)
		parent[i] = i;

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		for (j = i + 1; j < NUMA_NODE_COUNT; j++) {
			if (hydra_topo.dist[i][j] <= thr) {
				int ri = hydra_domain_root(parent, i);
				int rj = hydra_domain_root(parent, j);

				if (ri != rj)
					parent[rj] = ri;
			}
		}
	}

	ndom = 0;
	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		int r = hydra_domain_root(parent, i);

		if (r == i)
			hydra_topo.domain[i] = ndom++;
		else
			hydra_topo.domain[i] = hydra_topo.domain[r];
	}
	hydra_topo.nr_domains = ndom;

	smp_wmb();
	hydra_topo.ready = true;

	pr_info("HYDRA: topology: %d nodes, %d domains, share/domain dist <= %d, min offnode dist %d\n",
		NUMA_NODE_COUNT, ndom, thr, hydra_topo.min_offnode_dist);
}

int hydra_topology_show(struct seq_file *m, void *v)
{
	char buf[12];
	int i, j;

	seq_puts(m, " Hydra replication topology\n");
	seq_printf(m, " %-28s %6d\n", "nodes", hydra_topo.nr_nodes);
	seq_printf(m, " %-28s %6d\n", "domains", hydra_topo.nr_domains);
	seq_printf(m, " %-28s %6d\n", "share/domain dist threshold",
		   hydra_topo.share_dist);
	seq_printf(m, " %-28s %6d\n", "min offnode dist",
		   hydra_topo.min_offnode_dist);
	seq_printf(m, " %-28s %6d\n", "share dist knob (-1 = auto)",
		   READ_ONCE(sysctl_hydra_share_dist));
	seq_printf(m, " %-28s %6d\n", "domain dist knob (0 = auto)",
		   READ_ONCE(sysctl_hydra_domain_dist));

	seq_puts(m, "\n SLIT distances  [rows = from node, cols = to node]\n");
	seq_puts(m, "     ");
	for (j = 0; j < NUMA_NODE_COUNT; j++) {
		scnprintf(buf, sizeof(buf), "n%d", j);
		seq_printf(m, " %4s", buf);
	}
	seq_putc(m, '\n');
	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		scnprintf(buf, sizeof(buf), "n%d", i);
		seq_printf(m, " %-4s", buf);
		for (j = 0; j < NUMA_NODE_COUNT; j++)
			seq_printf(m, " %4d", hydra_topo.dist[i][j]);
		seq_putc(m, '\n');
	}

	seq_puts(m, "\n node -> replication domain\n");
	for (i = 0; i < NUMA_NODE_COUNT; i++)
		seq_printf(m, " n%-3d %4d\n", i, hydra_topo.domain[i]);

	return 0;
}

static int __init hydra_topology_init(void)
{
	hydra_topology_update();
	return 0;
}
subsys_initcall(hydra_topology_init);
