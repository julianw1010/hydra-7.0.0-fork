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
#include <linux/sched/mm.h>
#include <linux/kthread.h>
#include <linux/uaccess.h>
#include <linux/mman.h>
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
		u32 local = 0;

		if (hydra_topo.source == HYDRA_TOPO_MEASURED) {
			for (j = 0; j < NUMA_NODE_COUNT; j++) {
				u32 v = hydra_topo.lat_ns[i][j];

				if (v && (!local || v < local))
					local = v;
			}
			if (!local)
				local = 1;
		}

		for (j = 0; j < NUMA_NODE_COUNT; j++) {
			int d;

			if (hydra_topo.source == HYDRA_TOPO_MEASURED) {
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

#define HYDRA_REAL_PAGES 65536UL

struct hydra_real_ctx {
	struct mm_struct *mm;
	unsigned long base;
	atomic_t gate;
	atomic_t barrier;
	atomic_t generation;
	atomic_t failed;
	int g;
};

static struct hydra_real_ctx hydra_real_state;

struct hydra_real_worker {
	struct work_struct work;
	int self;
};

static struct hydra_real_worker hydra_real_workers[NUMA_NODE_COUNT];

static void hydra_real_barrier(struct hydra_real_ctx *c)
{
	int gen = atomic_read(&c->generation);

	if (atomic_inc_return(&c->barrier) == c->g) {
		atomic_set(&c->barrier, 0);
		atomic_inc(&c->generation);
	} else {
		while (atomic_read(&c->generation) == gen)
			cpu_relax();
	}
}

static void hydra_real_fn(struct work_struct *w)
{
	struct hydra_real_worker *rw =
		container_of(w, struct hydra_real_worker, work);
	struct hydra_real_ctx *c = &hydra_real_state;
	unsigned long i;
	u64 sum = 0;

	kthread_use_mm(c->mm);

	while (!atomic_read(&c->gate))
		cond_resched();

	for (i = rw->self; i < HYDRA_REAL_PAGES; i += c->g) {
		u64 __user *p = (u64 __user *)(c->base + (i << PAGE_SHIFT));

		if (put_user((u64)i + 1, p)) {
			atomic_set(&c->failed, 1);
			break;
		}
	}

	hydra_real_barrier(c);

	for (i = 0; i < HYDRA_REAL_PAGES; i++) {
		u64 __user *p = (u64 __user *)(c->base + (i << PAGE_SHIFT));
		u64 v;

		if (get_user(v, p)) {
			atomic_set(&c->failed, 1);
			break;
		}
		sum += v;
	}

	kthread_unuse_mm(c->mm);

	if (sum == U64_MAX)
		atomic_set(&c->failed, 1);
}

static long hydra_real_mmap_fn(void *arg)
{
	struct hydra_real_ctx *c = arg;
	unsigned long addr;

	kthread_use_mm(c->mm);
	addr = vm_mmap(NULL, 0, HYDRA_REAL_PAGES << PAGE_SHIFT,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, 0);
	kthread_unuse_mm(c->mm);

	if (IS_ERR_VALUE(addr))
		return -ENOMEM;
	c->base = addr;
	return 0;
}

static int hydra_real_run(int *gnodes, int g, int share, u32 *ns_out)
{
	struct hydra_real_ctx *c = &hydra_real_state;
	int saved = hydra_topo.share_dist;
	u64 t0, t1;
	int i, ret;

	c->mm = mm_alloc();
	if (!c->mm)
		return -ENOMEM;

	hydra_topo.share_dist = share;

	ret = hydra_enable_replication(c->mm);
	if (ret)
		goto out;

	ret = work_on_cpu(cpumask_first(cpumask_of_node(gnodes[0])),
			  hydra_real_mmap_fn, c);
	if (ret)
		goto out;

	atomic_set(&c->gate, 0);
	atomic_set(&c->barrier, 0);
	atomic_set(&c->generation, 0);
	atomic_set(&c->failed, 0);
	c->g = g;

	for (i = 0; i < g; i++) {
		hydra_real_workers[i].self = i;
		INIT_WORK(&hydra_real_workers[i].work, hydra_real_fn);
		schedule_work_on(cpumask_first(cpumask_of_node(gnodes[i])),
				 &hydra_real_workers[i].work);
	}

	t0 = ktime_get_ns();
	atomic_set(&c->gate, 1);
	for (i = 0; i < g; i++)
		flush_work(&hydra_real_workers[i].work);
	t1 = ktime_get_ns();

	if (atomic_read(&c->failed))
		ret = -EFAULT;
	else
		*ns_out = div64_u64(t1 - t0, HYDRA_REAL_PAGES * (u64)g);

out:
	hydra_topo.share_dist = saved;
	mmput(c->mm);
	c->mm = NULL;
	return ret;
}

static void hydra_cal_contention(void)
{
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

	hydra_topo.nr_tiers = 0;
	for (t = 0; t < ntier; t++) {
		struct hydra_topo_tier *row;
		u32 sh = 0, rp = 0;
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

		if (hydra_real_run(gnodes, g, LOCAL_DISTANCE, &rp))
			continue;
		if (hydra_real_run(gnodes, g, tiers[t], &sh))
			continue;

		row = &hydra_topo.tier[hydra_topo.nr_tiers++];
		row->dist = tiers[t];
		row->group = g;
		row->shared_ns = sh;
		row->repl_ns = rp;

		if (sh <= rp)
			best = tiers[t];
		else
			break;
	}

	hydra_topo.share_dist = best;

	pr_info("HYDRA: real-workload calibration: auto share dist %d (cluster dist %d, %d tiers)\n",
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
		seq_puts(m, "\n real-workload calibration  [rows = candidate sharing tier, ns per walked page]\n");
		seq_printf(m, " %-10s %-6s %-12s %-11s %s\n",
			   "tier_dist", "group", "shared_ns", "repl_ns",
			   "verdict");
		for (i = 0; i < hydra_topo.nr_tiers; i++) {
			struct hydra_topo_tier *t = &hydra_topo.tier[i];

			seq_printf(m, " %-10d %-6d %-12u %-11u %s\n",
				   t->dist, t->group, t->shared_ns, t->repl_ns,
				   t->shared_ns <= t->repl_ns ?
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
