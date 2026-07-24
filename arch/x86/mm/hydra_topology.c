#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/topology.h>
#include <linux/nodemask.h>
#include <linux/seq_file.h>
#include <linux/sort.h>
#include <linux/gfp.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/cpumask.h>
#include <linux/vmalloc.h>
#include <linux/hydra.h>
#include <asm/cacheflush.h>

struct hydra_topology hydra_topo;

int sysctl_hydra_domain_dist;

static DEFINE_MUTEX(hydra_cal_mutex);

#define HYDRA_CAL_CHUNK_ORDER 10
#define HYDRA_CAL_CHUNKS 32
#define HYDRA_CAL_LINE_SHIFT 7
#define HYDRA_CAL_LINES_PER_CHUNK \
	((PAGE_SIZE << HYDRA_CAL_CHUNK_ORDER) >> HYDRA_CAL_LINE_SHIFT)
#define HYDRA_CAL_LINES (HYDRA_CAL_CHUNKS * HYDRA_CAL_LINES_PER_CHUNK)
#define HYDRA_CAL_BURST 16384
#define HYDRA_CAL_ROUNDS 8

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
			int d;

			if (hydra_topo.source == HYDRA_TOPO_MEASURED) {
				u32 local = hydra_topo.lat_ns[i][i] ? : 1;

				if (i == j)
					d = LOCAL_DISTANCE;
				else
					d = DIV_ROUND_CLOSEST(LOCAL_DISTANCE *
						hydra_topo.lat_ns[i][j], local);
				d = clamp(d, LOCAL_DISTANCE, 255);
			} else {
				d = node_distance(i, j);
				if (d < 0 || d > 255)
					d = 255;
			}
			hydra_topo.dist[i][j] = d;
			if (i != j && d < hydra_topo.min_offnode_dist)
				hydra_topo.min_offnode_dist = d;
		}
	}

	thr = READ_ONCE(sysctl_hydra_domain_dist);
	if (thr <= 0)
		thr = hydra_auto_threshold();
	hydra_topo.cluster_dist = thr;
	hydra_topo.share_dist = thr;
	hydra_topo.nr_tiers = 0;

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

	pr_info("HYDRA: topology (%s): %d nodes, %d domains, share/domain dist <= %d, min offnode dist %d\n",
		hydra_topo.source == HYDRA_TOPO_MEASURED ? "measured" : "SLIT",
		NUMA_NODE_COUNT, ndom, thr, hydra_topo.min_offnode_dist);
}

void hydra_topology_use_slit(void)
{
	mutex_lock(&hydra_cal_mutex);
	hydra_topo.source = HYDRA_TOPO_SLIT;
	hydra_topology_update();
	mutex_unlock(&hydra_cal_mutex);
}

struct hydra_cal_probe {
	void *start;
	struct page **chunks;
	u64 best_ns;
};

static long hydra_cal_chase(void *arg)
{
	struct hydra_cal_probe *pr = arg;
	unsigned long flags;
	u64 t0, t1;
	void *p = pr->start;
	int r, k, c;

	for (c = 0; c < HYDRA_CAL_CHUNKS; c++)
		clflush_cache_range(page_address(pr->chunks[c]),
				    PAGE_SIZE << HYDRA_CAL_CHUNK_ORDER);

	for (k = 0; k < 1024; k++)
		p = *(void **)p;

	pr->best_ns = U64_MAX;
	for (r = 0; r < HYDRA_CAL_ROUNDS; r++) {
		local_irq_save(flags);
		t0 = ktime_get_ns();
		for (k = 0; k < HYDRA_CAL_BURST; k++)
			p = *(void **)p;
		t1 = ktime_get_ns();
		local_irq_restore(flags);
		if (t1 - t0 < pr->best_ns)
			pr->best_ns = t1 - t0;
		cond_resched();
	}
	pr->start = p;
	return 0;
}

static void *hydra_cal_line(struct page **chunks, u32 idx)
{
	return page_address(chunks[idx / HYDRA_CAL_LINES_PER_CHUNK]) +
	       ((unsigned long)(idx % HYDRA_CAL_LINES_PER_CHUNK) <<
		HYDRA_CAL_LINE_SHIFT);
}

static void *hydra_cal_build(struct page **chunks, u32 *perm)
{
	u32 i, k;

	for (i = 0; i < HYDRA_CAL_LINES; i++)
		perm[i] = i;
	for (i = HYDRA_CAL_LINES - 1; i > 0; i--) {
		u32 r = get_random_u32() % (i + 1);

		swap(perm[i], perm[r]);
	}

	for (k = 0; k < HYDRA_CAL_LINES; k++)
		*(void **)hydra_cal_line(chunks, perm[k]) =
			hydra_cal_line(chunks, perm[(k + 1) % HYDRA_CAL_LINES]);

	return hydra_cal_line(chunks, perm[0]);
}

#define HYDRA_CONT_OPS 65536
#define HYDRA_CONT_ROUNDS 3
#define HYDRA_CONT_SLOTS 512

struct hydra_cont_ant {
	struct work_struct work;
	atomic_t *gate;
	atomic_t *stop;
	void *target;
};

static void hydra_cont_ant_fn(struct work_struct *w)
{
	struct hydra_cont_ant *aw = container_of(w, struct hydra_cont_ant, work);
	u64 x = 0x2545F4914F6CDD1DULL * (smp_processor_id() + 3);
	int k;

	while (!atomic_read(aw->gate))
		cond_resched();

	while (!atomic_read(aw->stop)) {
		for (k = 0; k < 4096; k++) {
			unsigned long idx;

			x = x * 6364136223846793005ULL + 1442695040888963407ULL;
			idx = (x >> 33) & (HYDRA_CONT_SLOTS - 1);
			atomic_long_or(0,
				(atomic_long_t *)((u64 *)aw->target + idx));
		}
		cond_resched();
	}
}

struct hydra_cont_meas {
	void *chase;
	void *sink;
	void **fanout;
	int nfan;
	u64 chase_ns;
	u64 fanout_ns;
};

static long hydra_cont_meas_fn(void *arg)
{
	struct hydra_cont_meas *mp = arg;
	void *p;
	u64 t0, t1, best = U64_MAX;
	int r, k, j;

	for (r = 0; r < HYDRA_CONT_ROUNDS; r++) {
		p = mp->chase;
		t0 = ktime_get_ns();
		for (k = 0; k < HYDRA_CONT_OPS; k++)
			p = *(void **)p;
		t1 = ktime_get_ns();
		if (t1 - t0 < best)
			best = t1 - t0;
		mp->sink = p;
	}
	mp->chase_ns = div64_u64(best, HYDRA_CONT_OPS);

	if (mp->nfan) {
		u64 x = 0x9E3779B97F4A7C15ULL;

		best = U64_MAX;
		for (r = 0; r < HYDRA_CONT_ROUNDS; r++) {
			t0 = ktime_get_ns();
			for (k = 0; k < HYDRA_CONT_OPS; k++) {
				unsigned long idx;

				x = x * 6364136223846793005ULL +
				    1442695040888963407ULL;
				idx = (x >> 33) & (HYDRA_CONT_SLOTS - 1);
				for (j = 0; j < mp->nfan; j++)
					WRITE_ONCE(*((u64 *)mp->fanout[j] + idx), x);
			}
			t1 = ktime_get_ns();
			if (t1 - t0 < best)
				best = t1 - t0;
		}
		mp->fanout_ns = div64_u64(best, HYDRA_CONT_OPS);
	}

	return 0;
}

static void hydra_cont_chain(void *base)
{
	static u32 perm[HYDRA_CONT_SLOTS];
	u32 i;

	for (i = 0; i < HYDRA_CONT_SLOTS; i++)
		perm[i] = i;
	for (i = HYDRA_CONT_SLOTS - 1; i > 0; i--) {
		u32 r = get_random_u32() % (i + 1);

		swap(perm[i], perm[r]);
	}
	for (i = 0; i < HYDRA_CONT_SLOTS; i++)
		*(void **)((u64 *)base + perm[i]) =
			(u64 *)base + perm[(i + 1) % HYDRA_CONT_SLOTS];
}

static struct hydra_cont_ant hydra_cont_ants[NUMA_NODE_COUNT];
static void *hydra_cont_fan_pages[NUMA_NODE_COUNT];

static int hydra_cont_tier(int *gnodes, int g, struct page **npages,
			   u32 *pen_out, u32 *fan_out)
{
	struct hydra_cont_ant *ant = hydra_cont_ants;
	struct hydra_cont_meas meas;
	void **fanout = hydra_cont_fan_pages;
	atomic_t gate, stop;
	int meas_idx = g - 1;
	int mnode = gnodes[meas_idx];
	int mcpu = cpumask_first(cpumask_of_node(mnode));
	u64 shared_ns, priv_ns;
	int i, nant, ret;

	hydra_cont_chain(page_address(npages[gnodes[0]]));

	atomic_set(&gate, 0);
	atomic_set(&stop, 0);
	nant = 0;
	for (i = 0; i < g; i++) {
		if (i == meas_idx)
			continue;
		INIT_WORK(&ant[nant].work, hydra_cont_ant_fn);
		ant[nant].gate = &gate;
		ant[nant].stop = &stop;
		ant[nant].target = page_address(npages[gnodes[0]]);
		schedule_work_on(cpumask_first(cpumask_of_node(gnodes[i])),
				 &ant[nant].work);
		nant++;
	}
	atomic_set(&gate, 1);

	memset(&meas, 0, sizeof(meas));
	meas.chase = page_address(npages[gnodes[0]]);
	ret = work_on_cpu(mcpu, hydra_cont_meas_fn, &meas);
	atomic_set(&stop, 1);
	for (i = 0; i < nant; i++) {
		flush_work(&ant[i].work);
	}
	if (ret)
		return ret;
	shared_ns = meas.chase_ns;

	hydra_cont_chain(page_address(npages[mnode]));

	atomic_set(&gate, 0);
	atomic_set(&stop, 0);
	nant = 0;
	for (i = 0; i < g; i++) {
		if (i == meas_idx)
			continue;
		INIT_WORK(&ant[nant].work, hydra_cont_ant_fn);
		ant[nant].gate = &gate;
		ant[nant].stop = &stop;
		ant[nant].target = page_address(npages[gnodes[i]]);
		schedule_work_on(cpumask_first(cpumask_of_node(gnodes[i])),
				 &ant[nant].work);
		nant++;
	}
	atomic_set(&gate, 1);

	memset(&meas, 0, sizeof(meas));
	meas.chase = page_address(npages[mnode]);
	for (i = 0; i < g - 1; i++)
		fanout[i] = page_address(npages[gnodes[i]]);
	meas.fanout = fanout;
	meas.nfan = g - 1;
	ret = work_on_cpu(mcpu, hydra_cont_meas_fn, &meas);
	atomic_set(&stop, 1);
	for (i = 0; i < nant; i++) {
		flush_work(&ant[i].work);
	}
	if (ret)
		return ret;
	priv_ns = meas.chase_ns;

	*pen_out = shared_ns > priv_ns ? shared_ns - priv_ns : 0;
	*fan_out = meas.fanout_ns;
	return 0;
}

static void hydra_cal_contention(void)
{
	struct page *npages[NUMA_NODE_COUNT];
	int gnodes[NUMA_NODE_COUNT];
	int tiers[8];
	int ntier = 0, best = LOCAL_DISTANCE;
	int i, j, t;

	for (j = 0; j < NUMA_NODE_COUNT; j++) {
		int d = hydra_topo.dist[0][j];
		bool seen = false;

		if (j == 0 || d > hydra_topo.cluster_dist)
			continue;
		for (t = 0; t < ntier; t++) {
			if (tiers[t] == d)
				seen = true;
		}
		if (!seen && ntier < 8)
			tiers[ntier++] = d;
	}

	if (!ntier) {
		hydra_topo.nr_tiers = 0;
		return;
	}

	for (i = 0; i < ntier; i++) {
		for (j = i + 1; j < ntier; j++) {
			if (tiers[j] < tiers[i])
				swap(tiers[i], tiers[j]);
		}
	}

	for (j = 0; j < NUMA_NODE_COUNT; j++) {
		npages[j] = alloc_pages_node(j,
			GFP_KERNEL | __GFP_ZERO | __GFP_THISNODE, 0);
		if (!npages[j] || page_to_nid(npages[j]) != j) {
			if (npages[j])
				__free_page(npages[j]);
			while (j--)
				__free_page(npages[j]);
			pr_info("HYDRA: contention probe: page alloc failed, share stays at cluster dist\n");
			return;
		}
	}

	hydra_topo.nr_tiers = 0;
	for (t = 0; t < ntier; t++) {
		struct hydra_topo_tier *row;
		u32 pen = 0, fan = 0;
		int g = 0, a, b;

		for (j = 0; j < NUMA_NODE_COUNT; j++) {
			if (hydra_topo.dist[0][j] <= tiers[t])
				gnodes[g++] = j;
		}
		if (g < 2)
			continue;

		for (a = 0; a < g; a++) {
			for (b = a + 1; b < g; b++) {
				if (hydra_topo.dist[0][gnodes[b]] <
				    hydra_topo.dist[0][gnodes[a]])
					swap(gnodes[a], gnodes[b]);
			}
		}

		if (hydra_cont_tier(gnodes, g, npages, &pen, &fan))
			continue;

		row = &hydra_topo.tier[hydra_topo.nr_tiers++];
		row->dist = tiers[t];
		row->group = g;
		row->shared_ns = pen;
		row->repl_ns = fan;

		if ((u64)pen * g <= fan)
			best = tiers[t];
		else
			break;
	}

	hydra_topo.share_dist = best;

	for (j = 0; j < NUMA_NODE_COUNT; j++)
		__free_page(npages[j]);

	pr_info("HYDRA: contention probe: auto share dist %d (cluster dist %d, %d tiers)\n",
		best, hydra_topo.cluster_dist, hydra_topo.nr_tiers);
}

int hydra_topology_calibrate(void)
{
	struct page *chunks[HYDRA_CAL_CHUNKS];
	u32 lat[NUMA_NODE_COUNT][NUMA_NODE_COUNT];
	u32 *perm;
	int i, j, k, ret = 0;

	perm = kvmalloc_array(HYDRA_CAL_LINES, sizeof(u32), GFP_KERNEL);
	if (!perm)
		return -ENOMEM;

	mutex_lock(&hydra_cal_mutex);

	for (i = 0; i < NUMA_NODE_COUNT; i++) {
		if (!node_online(i) ||
		    cpumask_first(cpumask_of_node(i)) >= nr_cpu_ids) {
			pr_info("HYDRA: calibrate: node %d offline or without CPUs, keeping SLIT\n", i);
			ret = -EINVAL;
			goto out;
		}
	}

	for (j = 0; j < NUMA_NODE_COUNT; j++) {
		void *start;

		for (k = 0; k < HYDRA_CAL_CHUNKS; k++) {
			chunks[k] = alloc_pages_node(j,
				GFP_KERNEL | __GFP_THISNODE,
				HYDRA_CAL_CHUNK_ORDER);
			if (!chunks[k] || page_to_nid(chunks[k]) != j) {
				if (chunks[k])
					__free_pages(chunks[k],
						     HYDRA_CAL_CHUNK_ORDER);
				while (k--)
					__free_pages(chunks[k],
						     HYDRA_CAL_CHUNK_ORDER);
				pr_info("HYDRA: calibrate: cannot allocate probe buffer on node %d\n", j);
				ret = -ENOMEM;
				goto out;
			}
		}

		start = hydra_cal_build(chunks, perm);

		for (i = 0; i < NUMA_NODE_COUNT; i++) {
			struct hydra_cal_probe probe = {
				.start = start,
				.chunks = chunks,
			};
			int cpu = cpumask_first(cpumask_of_node(i));

			ret = work_on_cpu(cpu, hydra_cal_chase, &probe);
			if (ret) {
				for (k = 0; k < HYDRA_CAL_CHUNKS; k++)
					__free_pages(chunks[k],
						     HYDRA_CAL_CHUNK_ORDER);
				goto out;
			}
			lat[i][j] = max_t(u32, 1,
					  div64_u64(probe.best_ns,
						    HYDRA_CAL_BURST));
		}

		for (k = 0; k < HYDRA_CAL_CHUNKS; k++)
			__free_pages(chunks[k], HYDRA_CAL_CHUNK_ORDER);
	}

	memcpy(hydra_topo.lat_ns, lat, sizeof(lat));
	hydra_topo.source = HYDRA_TOPO_MEASURED;
	hydra_topology_update();
	hydra_cal_contention();

out:
	mutex_unlock(&hydra_cal_mutex);
	kvfree(perm);
	return ret;
}

static void hydra_autocal_work_fn(struct work_struct *w)
{
	if (hydra_topology_calibrate())
		pr_info("HYDRA: boot auto-calibration failed, keeping SLIT topology\n");
}

static DECLARE_WORK(hydra_autocal_work, hydra_autocal_work_fn);

static int __init hydra_autocal_init(void)
{
	schedule_work(&hydra_autocal_work);
	return 0;
}
late_initcall_sync(hydra_autocal_init);

int hydra_topology_show(struct seq_file *m, void *v)
{
	char buf[12];
	int i, j;

	seq_puts(m, " Hydra replication topology\n");
	seq_printf(m, " %-28s %6d\n", "nodes", hydra_topo.nr_nodes);
	seq_printf(m, " %-28s %6d\n", "domains", hydra_topo.nr_domains);
	seq_printf(m, " %-28s %6d\n", "cluster (domain) dist",
		   hydra_topo.cluster_dist);
	seq_printf(m, " %-28s %6d\n", "auto share dist",
		   hydra_topo.share_dist);
	seq_printf(m, " %-28s %6d\n", "min offnode dist",
		   hydra_topo.min_offnode_dist);
	seq_printf(m, " %-28s %6d\n", "share dist knob (-1 = auto)",
		   READ_ONCE(sysctl_hydra_share_dist));
	seq_printf(m, " %-28s %6d\n", "domain dist knob (0 = auto)",
		   READ_ONCE(sysctl_hydra_domain_dist));
	seq_printf(m, " %-28s %6s\n", "distance source",
		   hydra_topo.source == HYDRA_TOPO_MEASURED ? "meas" : "SLIT");

	seq_printf(m, "\n active distances (%s)  [rows = from node, cols = to node]\n",
		   hydra_topo.source == HYDRA_TOPO_MEASURED ?
		   "measured, normalized local=10" : "SLIT");
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

	if (hydra_topo.source == HYDRA_TOPO_MEASURED) {
		seq_puts(m, "\n measured latency (ns/access)  [rows = from node, cols = to node]\n");
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
				seq_printf(m, " %4u", hydra_topo.lat_ns[i][j]);
			seq_putc(m, '\n');
		}
	}

	if (hydra_topo.nr_tiers) {
		seq_puts(m, "\n contention probe  [rows = candidate sharing tier]\n");
		seq_printf(m, " %-10s %-6s %-12s %-11s %s\n",
			   "tier_dist", "group", "walkpen_ns", "fanout_ns",
			   "verdict");
		for (i = 0; i < hydra_topo.nr_tiers; i++) {
			struct hydra_topo_tier *t = &hydra_topo.tier[i];

			seq_printf(m, " %-10d %-6d %-12u %-11u %s\n",
				   t->dist, t->group, t->shared_ns, t->repl_ns,
				   (u64)t->shared_ns * t->group <= t->repl_ns ?
				   "share" : "copy");
		}
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
