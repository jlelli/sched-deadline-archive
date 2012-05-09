/*
 * Real-Time Scheduling Class (mapped to the SCHED_FIFO and SCHED_RR
 * policies)
 */

#include "sched.h"

#include <linux/slab.h>

struct rt_bandwidth def_rt_bandwidth;

void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime)
{
	rt_b->rt_period = ns_to_ktime(period);
	rt_b->rt_runtime = runtime;

	raw_spin_lock_init(&rt_b->rt_runtime_lock);
}

static enum hrtimer_restart sched_rt_period_timer(struct hrtimer *hrtimer);

void init_rt_rq(struct rt_rq *rt_rq, struct rq *rq,
		struct rt_bandwidth *rt_b)
{
	struct rt_prio_array *array;
	int i;

	array = &rt_rq->active;
	for (i = 0; i < MAX_RT_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	/* delimiter for bitsearch: */
	__set_bit(MAX_RT_PRIO, array->bitmap);

	rt_rq->highest_prio = MAX_RT_PRIO;

	rt_rq->rt_time = 0;
	rt_rq->rt_throttled = 0;
	rt_rq->rt_deadline = 0;
	rt_rq->rt_runtime = rt_b->rt_runtime;
	rt_rq->rt_period = rt_b->rt_period;
	raw_spin_lock_init(&rt_rq->rt_runtime_lock);

	hrtimer_init(&rt_rq->rt_period_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	rt_rq->rt_period_timer.function = sched_rt_period_timer;

	rt_rq->rt_bw = to_ratio(ktime_to_ns(rt_b->rt_period), rt_b->rt_runtime);

#ifdef CONFIG_RT_GROUP_SCHED
	rt_rq->rt_nr_boosted = 0;
	rt_rq->rq = rq;
	rt_rq->rt_needs_resync = false;
#endif
}

#ifdef CONFIG_SMP
static inline unsigned long rt_init_free_bw(void)
{
	unsigned long used = to_ratio(global_rt_period(), global_rt_runtime());

	return to_ratio(RUNTIME_INF, RUNTIME_INF) - used;
}
#endif

void init_rt_root_rq(struct rt_root_rq *rt, struct rq *rq)
{
#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
	rt->highest_prio.curr = MAX_RT_PRIO;
#ifdef CONFIG_SMP
	rt->highest_prio.next = MAX_RT_PRIO;
#endif
#endif
#ifdef CONFIG_SMP
	rt->rt_nr_migratory = 0;
	rt->overloaded = 0;
	plist_head_init(&rt->pushable_tasks);

	rt->rt_edf_tree.rt_free_bw = rt_init_free_bw();
	raw_spin_lock_init(&rt->rt_edf_tree.rt_bw_lock);
#endif
	rt->rt_edf_tree.rb_root = RB_ROOT;
	init_rt_rq(&rt->rt_rq, rq, &def_rt_bandwidth);
}

#ifdef CONFIG_RT_GROUP_SCHED

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return rt_rq->rq;
}

struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	return rt_se->rt_rq;
}

void free_rt_sched_group(struct task_group *tg)
{
	struct rt_rq *rt_rq;
	int i;

	if (!tg->rt_rq)
		return;

	for_each_possible_cpu(i) {
		rt_rq = tg->rt_rq[i];

		if (rt_rq) {
			hrtimer_cancel(&rt_rq->rt_period_timer);
			kfree(rt_rq);
		}
	}

	kfree(tg->rt_rq);
}

void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq, int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	tg->rt_rq[cpu] = rt_rq;

	init_rt_rq(rt_rq, rq, &tg->rt_task_bandwidth);
	rt_rq->tg = tg;

	RB_CLEAR_NODE(&rt_rq->rb_node);
}

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct rt_rq *rt_rq;
	int i;

	tg->rt_rq = kzalloc(sizeof(rt_rq) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->rt_rq)
		goto err;

	init_rt_bandwidth(&tg->rt_bandwidth,
			ktime_to_ns(parent->rt_bandwidth.rt_period), 0);
	init_rt_bandwidth(&tg->rt_task_bandwidth,
			ktime_to_ns(parent->rt_bandwidth.rt_period), 0);

	for_each_possible_cpu(i) {
		rt_rq = kzalloc_node(sizeof(struct rt_rq),
				     GFP_KERNEL, cpu_to_node(i));
		if (!rt_rq)
			goto err;

		init_tg_rt_entry(tg, rt_rq, i);
	}

	return 1;

err:
	return 0;
}

#else /* CONFIG_RT_GROUP_SCHED */

static inline struct task_struct *rt_task_of(struct sched_rt_entity *rt_se)
{
	return container_of(rt_se, struct task_struct, rt);
}

static inline struct rq *rq_of_rt_rq(struct rt_rq *rt_rq)
{
	return container_of(rt_rq, struct rq, rt.rt_rq);
}

static inline struct rt_rq *rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct task_struct *p = rt_task_of(rt_se);
	struct rq *rq = task_rq(p);

	return &rq->rt.rt_rq;
}

void free_rt_sched_group(struct task_group *tg) { }

int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP

static inline int rt_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->rto_count);
}

static inline void rt_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->rto_mask);
	/*
	 * Make sure the mask is visible before we set
	 * the overload count. That is checked to determine
	 * if we should look at the mask. It would be a shame
	 * if we looked at the mask, but the mask was not
	 * updated yet.
	 */
	wmb();
	atomic_inc(&rq->rd->rto_count);
}

static inline void rt_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	/* the order here really doesn't matter */
	atomic_dec(&rq->rd->rto_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->rto_mask);
}

static void update_rt_migration(struct rt_root_rq *rt)
{
	struct rq *rq = container_of(rt, struct rq, rt);

	if (rt->rt_nr_migratory && rt->rt_nr_total > 1) {
		if (!rt->overloaded) {
			rt_set_overload(rq);
			rt->overloaded = 1;
		}
	} else if (rt->overloaded) {
		rt_clear_overload(rq);
		rt->overloaded = 0;
	}
}

static void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;
	struct rt_root_rq *rt;

	p = rt_task_of(rt_se);
	rt = &rq_of_rt_rq(rt_rq)->rt;

	rt->rt_nr_total++;
	if (p->nr_cpus_allowed > 1)
		rt->rt_nr_migratory++;

	update_rt_migration(rt);
}

static void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	struct task_struct *p;
	struct rt_root_rq *rt;

	p = rt_task_of(rt_se);
	rt = &rq_of_rt_rq(rt_rq)->rt;

	rt->rt_nr_total--;
	if (p->nr_cpus_allowed > 1)
		rt->rt_nr_migratory--;

	update_rt_migration(rt);
}

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->rt.pushable_tasks);
}

static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the highest prio pushable task */
	if (p->prio < rq->rt.highest_prio.next)
		rq->rt.highest_prio.next = p->prio;
}

static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->rt.pushable_tasks);

	/* Update the new highest prio pushable task */
	if (has_pushable_tasks(rq)) {
		p = plist_first_entry(&rq->rt.pushable_tasks,
				      struct task_struct, pushable_tasks);
		rq->rt.highest_prio.next = p->prio;
	} else
		rq->rt.highest_prio.next = MAX_RT_PRIO;
}

#else

static inline void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

static inline
void dec_rt_migration(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
}

#endif /* CONFIG_SMP */

static inline int on_rt_rq(struct sched_rt_entity *rt_se)
{
	return !list_empty(&rt_se->run_list);
}

static inline int rt_rq_on_rq(struct rt_rq *rt_rq)
{
	return !RB_EMPTY_NODE(&rt_rq->rb_node);
}

static inline int rt_rq_is_leftmost(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	return rq->rt.rt_edf_tree.rb_leftmost == &rt_rq->rb_node;
}

#ifdef CONFIG_RT_GROUP_SCHED

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	if (!rt_rq->tg)
		return RUNTIME_INF;

	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(rt_rq->tg->rt_task_bandwidth.rt_period);
}

typedef struct task_group *rt_rq_iter_t;

static inline struct task_group *next_task_group(struct task_group *tg)
{
	do {
		tg = list_entry_rcu(tg->list.next,
			typeof(struct task_group), list);
	} while (&tg->list != &task_groups && task_group_is_autogroup(tg));

	if (&tg->list == &task_groups)
		tg = NULL;

	return tg;
}

#define for_each_rt_rq(rt_rq, iter, rq)					\
	for (iter = container_of(&task_groups, typeof(*iter), list);	\
		(iter = next_task_group(iter)) &&			\
		(rt_rq = iter->rt_rq[cpu_of(rq)]);)

static inline void list_add_leaf_rt_rq(struct rt_rq *rt_rq)
{
	list_add_rcu(&rt_rq->leaf_rt_rq_list,
			&rq_of_rt_rq(rt_rq)->leaf_rt_rq_list);
}

static inline void list_del_leaf_rt_rq(struct rt_rq *rt_rq)
{
	list_del_rcu(&rt_rq->leaf_rt_rq_list);
}

#define for_each_leaf_rt_rq(rt_rq, rq) \
	list_for_each_entry_rcu(rt_rq, &rq->leaf_rt_rq_list, leaf_rt_rq_list)

static void __enqueue_rt_rq(struct rt_rq *rt_rq);
static void __dequeue_rt_rq(struct rt_rq *rt_rq);

static void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);	/* check for races */

	if (rt_rq->rt_nr_running && !rt_rq_on_rq(rt_rq)) {
		__enqueue_rt_rq(rt_rq);
		if (rt_rq_is_leftmost(rt_rq))
			resched_task(rq->curr);
	}
}

static void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
	if (rt_rq_on_rq(rt_rq))
		__dequeue_rt_rq(rt_rq);
}

static inline void sched_rt_deadline_updated(struct rt_rq *rt_rq)
{
	int was_leftmost = rt_rq_is_leftmost(rt_rq);

	sched_rt_rq_dequeue(rt_rq);
	sched_rt_rq_enqueue(rt_rq);

	if (was_leftmost || rt_rq_is_leftmost(rt_rq))
		resched_task(rq_of_rt_rq(rt_rq)->curr);
}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled && !rt_rq->rt_nr_boosted;
}

static int rt_se_boosted(struct sched_rt_entity *rt_se)
{
	struct task_struct *p;

	p = rt_task_of(rt_se);
	return p->prio != p->normal_prio;
}

#ifdef CONFIG_SMP
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_rq(smp_processor_id())->rd->span;
}
#else
static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}
#endif

/*
 * Returns the rt_rq (on CPU cpu) that is part of the
 * task group associated with rt_b.
 */
static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return container_of(rt_b, struct task_group,
			    rt_task_bandwidth)->rt_rq[cpu];
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &rt_rq->tg->rt_task_bandwidth;
}

static inline void rt_period_set_expires(struct rt_rq *rt_rq, bool force)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	ktime_t now, dline, delta;

	if (hrtimer_active(&rt_rq->rt_period_timer) && !force)
		return;

	/*
	 * Compensate for discrepancies between rq->clock (used to
	 * calculate deadlines) and the hrtimer-measured time, to obtain
	 * a better absolute time instant for the timer itself.
	 */
	now = hrtimer_cb_get_time(&rt_rq->rt_period_timer);
	delta = ktime_sub_ns(now, rq->clock);
	dline = ktime_add_ns(delta, rt_rq->rt_deadline);
	hrtimer_set_expires(&rt_rq->rt_period_timer, dline);
}

static bool rt_rq_recharge(struct rt_rq *rt_rq, int overrun);

static inline void rt_compensate_overruns(struct rt_rq *rt_rq, int overrun)
{
	if (unlikely(overrun))
		rt_rq_recharge(rt_rq, overrun);
}

static struct rt_rq *pick_next_rt_rq(struct rq *rq)
{
	struct rb_node *left = rq->rt.rt_edf_tree.rb_leftmost;

	if (!left)
		return NULL;

	return rb_entry(left, struct rt_rq, rb_node);
}

#else /* !CONFIG_RT_GROUP_SCHED */

static inline u64 sched_rt_runtime(struct rt_rq *rt_rq)
{
	return rt_rq->rt_runtime;
}

static inline u64 sched_rt_period(struct rt_rq *rt_rq)
{
	return ktime_to_ns(def_rt_bandwidth.rt_period);
}

typedef struct rt_rq *rt_rq_iter_t;

#define for_each_rt_rq(rt_rq, iter, rq) \
	for ((void) iter, rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

static inline void list_add_leaf_rt_rq(struct rt_rq *rt_rq)
{
}

static inline void list_del_leaf_rt_rq(struct rt_rq *rt_rq)
{
}

#define for_each_leaf_rt_rq(rt_rq, rq) \
	for (rt_rq = &rq->rt; rt_rq; rt_rq = NULL)

static inline void sched_rt_rq_enqueue(struct rt_rq *rt_rq)
{
	if (rt_rq->rt_nr_running)
		resched_task(rq_of_rt_rq(rt_rq)->curr);
}

static inline void sched_rt_rq_dequeue(struct rt_rq *rt_rq)
{
}

static inline void sched_rt_deadline_updated(struct rt_rq *rt_rq) {}

static inline int rt_rq_throttled(struct rt_rq *rt_rq)
{
	return rt_rq->rt_throttled;
}

static inline const struct cpumask *sched_rt_period_mask(void)
{
	return cpu_online_mask;
}

static inline
struct rt_rq *sched_rt_period_rt_rq(struct rt_bandwidth *rt_b, int cpu)
{
	return &cpu_rq(cpu)->rt.rt_rq;
}

static inline struct rt_bandwidth *sched_rt_bandwidth(struct rt_rq *rt_rq)
{
	return &def_rt_bandwidth;
}

static inline void rt_period_set_expires(struct rt_rq *rt_rq, bool force) {}
static inline void rt_compensate_overruns(struct rt_rq *rt_rq, int overrun) {}

static struct rt_rq *pick_next_rt_rq(struct rq *rq)
{
	struct rt_rq *rt_rq = &rq->rt.rt_rq;

	if (!rt_rq->rt_nr_running || rt_rq_throttled(rt_rq))
		return NULL;

	return rt_rq;
}

#endif /* CONFIG_RT_GROUP_SCHED */

static inline int rt_time_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

#ifdef CONFIG_SMP
/*
 * Reset the balancing machinery, restarting from a safe runtime assignment
 * on all the cpus/rt_rqs in the system.  There is room for improvements here,
 * as this iterates through all the rt_rqs in the system; the main problem
 * is that after the balancing has been running for some time we are not
 * sure that the fragmentation of the free bandwidth it produced allows new
 * groups to run where they need to run.  The caller has to make sure that
 * only one instance of this function is running at any time.
 */
static void __rt_reset_runtime(void)
{
	struct rq *rq;
	struct rt_rq *rt_rq;
	struct rt_bandwidth *rt_b;
	rt_rq_iter_t iter;
	int i;

	for_each_possible_cpu(i) {
		rq = cpu_rq(i);

		rq->rt_balancing_disabled = 1;
		/*
		 * No memory barrier is necessary here.  The following
		 * lock+unlock sequences act as implicit barriers; all
		 * the new balancing operations occuring after the lock+
		 * unlock sequence will see the rt_balancing_disabled
		 * flag set, while the ones occurring before it will be
		 * safe because under the spinlock.
		 */
		for_each_rt_rq(rt_rq, iter, rq) {
			rt_b = sched_rt_bandwidth(rt_rq);

			raw_spin_lock_irq(&rt_b->rt_runtime_lock);
			raw_spin_lock(&rt_rq->rt_runtime_lock);
			rt_rq->rt_runtime = rt_b->rt_runtime;
			rt_rq->rt_period = rt_b->rt_period;
			rt_rq->rt_time = 0;
			rt_rq->rt_bw = to_ratio(ktime_to_ns(rt_b->rt_period),
						rt_b->rt_runtime);
			raw_spin_unlock(&rt_rq->rt_runtime_lock);
			raw_spin_unlock_irq(&rt_b->rt_runtime_lock);
		}
	}
}

#ifdef CONFIG_RT_GROUP_SCHED

static unsigned long rt_used_bandwidth(void)
{
	struct task_group *tg;
	unsigned long used = 0;
	u64 rt_period;

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		rt_period = ktime_to_ns(tg->rt_task_bandwidth.rt_period);
		used += to_ratio(rt_period, tg->rt_task_bandwidth.rt_runtime);
	}
	rcu_read_unlock();

	return used;
}

#else

static unsigned long rt_used_bandwidth(void)
{
	return to_ratio(global_rt_period(), global_rt_runtime());
}

#endif

/*
 * Maximum bandwidth available for rt computing on a singled CPU.  To
 * preserve the old behaviour, allow them to use all the bandwidth;
 * the runtime migration code makes sure that, if a CPU is saturated,
 * CPU time is still available for non-rt tasks on the other CPUs.
 */
static unsigned long rt_free_bandwidth(void)
{
	return to_ratio(RUNTIME_INF, RUNTIME_INF);
}

static void __rt_restart_balancing(void)
{
	unsigned long free;
	struct rq *rq;
	int i;

	free = rt_free_bandwidth() - rt_used_bandwidth();

	for_each_possible_cpu(i) {
		rq = cpu_rq(i);

		raw_spin_lock_irq(&rq->rt.rt_edf_tree.rt_bw_lock);
		rq->rt.rt_edf_tree.rt_free_bw = free;
		raw_spin_unlock_irq(&rq->rt.rt_edf_tree.rt_bw_lock);

		rq->rt_balancing_disabled = 0;
	}
}

void rt_reset_runtime(void)
{
	__rt_reset_runtime();
	__rt_restart_balancing();
}

static inline void double_spin_lock(raw_spinlock_t *lock1,
				    raw_spinlock_t *lock2)
	__acquires(lock1)
	__acquires(lock2)
{
	if (lock1 < lock2) {
		raw_spin_lock(lock1);
		raw_spin_lock_nested(lock2, SINGLE_DEPTH_NESTING);
	} else {
		raw_spin_lock(lock2);
		raw_spin_lock_nested(lock1, SINGLE_DEPTH_NESTING);
	}
}

static inline void double_spin_unlock(raw_spinlock_t *lock1,
				      raw_spinlock_t *lock2)
	__releases(lock1)
	__releases(lock2)
{
	raw_spin_unlock(lock1);
	raw_spin_unlock(lock2);
}

/*
 * Keep track of the bandwidth allocated to each rt_rq.  To avoid accumulating
 * the error introduced by to_ratio() and from_ratio() we keep track of the
 * contribution to rt_free_bw from each rt_rq, and we sum it back before
 * subtracting the new contribution.
 */ 
static inline void rt_update_bw(struct rt_rq *rt_rq, struct rt_edf_tree *tree,
				s64 diff, u64 rt_period)
{
	unsigned long bw;

	rt_rq->rt_runtime += diff;
	bw = to_ratio(rt_period, rt_rq->rt_runtime);
	tree->rt_free_bw += bw - rt_rq->rt_bw;
	rt_rq->rt_bw = bw;
}

/*
 * Try to move *diff units of runtime from src to dst, checking
 * that the utilization does not exceed the global limits on the
 * destination cpu.  Returns true if the migration succeeded, leaving
 * in *diff the actual amount of runtime moved, false on failure, which
 * means that no more bandwidth can be migrated to rt_rq.
 */
static bool rt_move_bw(struct rt_rq *src, struct rt_rq *dst,
		       s64 *diff, u64 rt_period, bool force)
{
	struct rq *rq = rq_of_rt_rq(dst), *src_rq = rq_of_rt_rq(src);
	struct rt_edf_tree *dtree = &rq->rt.rt_edf_tree;
	struct rt_edf_tree *stree = &src_rq->rt.rt_edf_tree;
	unsigned long bw_to_move;
	bool ret = false;

	double_spin_lock(&dtree->rt_bw_lock, &stree->rt_bw_lock);

	if (dtree->rt_free_bw || force) {
		bw_to_move = to_ratio(rt_period, *diff);
		if (bw_to_move > dtree->rt_free_bw && !force)
			*diff = from_ratio(dtree->rt_free_bw, rt_period);

		if (*diff) {
			rt_update_bw(src, stree, -(*diff), rt_period);
			rt_update_bw(dst, dtree, *diff, rt_period);

			ret = true;
		}
	}

	double_spin_unlock(&dtree->rt_bw_lock, &stree->rt_bw_lock);

	return ret;
}

static inline bool runtime_push_needed(struct rt_rq *src, struct rt_rq *dst)
{
	if (!dst->rt_nr_running)
		return false;

	if (src->rt_runtime <= dst->rt_runtime)
		return false;

	return true;
}

static inline bool stop_runtime_balancing(bool moved, struct rt_rq *dst,
					  u64 target)
{
	if (!moved) {
		/* No more available bw on our cpu while pulling. */
		return true;
	}

	if (dst->rt_runtime >= target) {
		/* We reached our goal. */
		return true;
	}

	return false;
}

/*
 * Try to move runtime between rt_rq and iter, in the direction specified
 * by pull.  Return true if balancing should stop.
 */
static inline bool move_runtime(struct rt_rq *rt_rq, struct rt_rq *iter,
				u64 target, int weight, u64 min_runtime,
				u64 rt_period)
{
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
	struct rt_rq *src = iter, *dst = rt_rq;
	u64 leave;
	s64 diff = 0;
	bool moved = true;

	/*
	 * From runqueues with spare time, take 1/n part of their
	 * spare time, but no more than our period.  In case we are
	 * stealing from an active rt_rq, make sure we steal only
	 * from the runtime it borrowed, to avoid instability.  We're
	 * also careful with runqueues waiting to be recharged, letting
	 * them get refills of at least rt_b->rt_runtime.
	 */
	if (iter->rt_nr_running || hrtimer_active(&rt_rq->rt_period_timer))
		leave = rt_b->rt_runtime;
	else
		leave = max(min_runtime, iter->rt_time);
	diff = iter->rt_runtime - leave;

	if (diff > 0) {
		diff = div_u64((u64)diff, weight);
		if (dst->rt_runtime + diff > rt_period)
			diff = rt_period - dst->rt_runtime;
		if (dst->rt_runtime + diff > target)
			diff = target - dst->rt_runtime;

		moved = rt_move_bw(src, dst, &diff, rt_period, false);
	}

	return stop_runtime_balancing(moved, dst, target);
}

/*
 * Handle runtime rebalancing: XXX
 */
static void do_balance_runtime(struct rt_rq *rt_rq)
{
	struct rq *rq = cpu_rq(smp_processor_id());
	struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
	struct root_domain *rd = rq->rd;
	int i, weight, tot_weight;
	u64 rt_period, prev_runtime, target, min_runtime;
	bool stop = false;

	raw_spin_lock(&rt_b->rt_runtime_lock);
	/*
	 * The raw_spin_lock() acts as an acquire barrier, ensuring
	 * that rt_balancing_disabled is accessed after taking the lock;
	 * since rt_reset_runtime() takes rt_runtime_lock after setting
	 * the disable flag we are sure that no bandwidth is migrated
	 * while the reset is in progress.
	 */
	if (rq->rt_balancing_disabled)
		goto unlock;

	tot_weight = cpumask_weight(rd->span);

	/*
	 * We want to leave some runtime to idle runqueues, otherwise their
	 * deadline could be postponed indefinitely.  Note that rebalancing
	 * before postponing the deadline could not be sufficient, because
	 * the rebalance would not be guaranteed to take place.
	 */
	min_runtime = div_u64(rt_b->rt_runtime, tot_weight);

	if (rt_rq->rt_runtime < rt_b->rt_runtime) {
		/* Try to recover all the runtime we've lent. */
		target = rt_b->rt_runtime;

		/* Take all we can for ourselves. */
		weight = 1;
	} else {
		/*
		 * Share the bandwidth that can be lent with the other
		 * active rt_rqs; the count is not accurate, as we haven't
		 * got a lock to globally protect the rt_nr_running counters,
		 * but if there are frequent activations/deactivations we
		 * can't obtain a steady balance anyway...
		 */
		weight = 0;
		for_each_cpu(i, rd->span) {
			struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
			weight += !!iter->rt_nr_running;
		}

		/* Aim at our share plus 1/weight of all the spare bw. */
		target = rt_b->rt_runtime + div_u64((tot_weight-weight)
				* (rt_b->rt_runtime - min_runtime), weight);
	}

	prev_runtime = rt_rq->rt_runtime;
	rt_period = ktime_to_ns(rt_b->rt_period);
	for_each_cpu(i, rd->span) {
		struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
		struct rq *iter_rq = rq_of_rt_rq(iter);

		if (iter == rt_rq)
			continue;

		if (iter_rq->rt_balancing_disabled)
			continue;

		raw_spin_lock(&iter->rt_runtime_lock);
		/*
		 * Either all rqs have inf runtime and there's nothing to steal
		 * or __disable_runtime() below sets a specific rq to inf to
		 * indicate its been disabled and disallow stealing.
		 */
		if (iter->rt_runtime == RUNTIME_INF)
			goto next;

		stop = move_runtime(rt_rq, iter, target, weight,
				    min_runtime, rt_period);

next:
		raw_spin_unlock(&iter->rt_runtime_lock);
		if (stop)
			break;
	}

#if 0
	printk("[%d] PRT=%ld NRT=%ld TGT=%ld W=%d NR=%ld\n", rq->cpu,
		(long)prev_runtime, (long)rt_rq->rt_runtime, (long)target,
		weight, (long)rt_rq->rt_nr_running);
#endif

	/*
	 * If the runqueue is not throttled, changing its runtime
	 * without updating its deadline could create transients during
	 * which the rt_rq uses more bandwidth than assigned.  Here vtime
	 * is the time instant when an ideal server with prev_runtime /
	 * rt_period utilization would have given the same amount of
	 * service to rt_rq as it actually received.  At this time instant
	 * it is true that rt_time = vtime * prev_runtime / rt_period,
	 * (the service in the ideal server grows linearly at the nominal
	 * rate allocated to rt_rq), so we can invert the relation to
	 * find vtime and calculate the new deadline from there.  If the
	 * vtime is in the past then rt_rq is lagging behind the ideal
	 * system and we cannot risk an overload afterwards, so we just
	 * restart from rq->clock.
	 */
	if (!rt_rq_throttled(rt_rq) && prev_runtime != rt_rq->rt_runtime) {
		u64 vtime = rt_rq->rt_deadline - rt_period +
			div64_u64(rt_rq->rt_time * rt_period, prev_runtime);

		if (rt_time_before(vtime, rq->clock))
			vtime = rq->clock;

		rt_rq->rt_deadline = vtime + rt_period;
		sched_rt_deadline_updated(rt_rq);
	}
unlock:
	raw_spin_unlock(&rt_b->rt_runtime_lock);
}

/*
 * Ensure this RQ takes back all the runtime it has lent to its neighbours.
 */
static void __disable_runtime(struct rq *rq)
{
	struct root_domain *rd = rq->rd;
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
		u64 rt_period = ktime_to_ns(rt_b->rt_period);
		s64 want;
		int i;

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * Either we're all inf and nobody needs to borrow, or we're
		 * already disabled and thus have nothing to do, or we have
		 * exactly the right amount of runtime to take out.
		 */
		if (rt_rq->rt_runtime == RUNTIME_INF ||
				rt_rq->rt_runtime == rt_b->rt_runtime)
			goto balanced;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);

		/*
		 * Calculate the difference between what we started out with
		 * and what we current have, that's the amount of runtime
		 * we lend and now have to reclaim.
		 */
		want = rt_b->rt_runtime - rt_rq->rt_runtime;

		/*
		 * Greedy reclaim, take back as much as we can.
		 */
		for_each_cpu(i, rd->span) {
			struct rt_rq *iter = sched_rt_period_rt_rq(rt_b, i);
			s64 diff;
			bool moved;

			/*
			 * Can't reclaim from ourselves or disabled runqueues.
			 */
			if (iter == rt_rq || iter->rt_runtime == RUNTIME_INF)
				continue;

			raw_spin_lock(&iter->rt_runtime_lock);
			if (want > 0) {
				diff = want;
				rt_move_bw(iter, rt_rq, &diff, rt_period, true);
				want -= diff;
			} else {
				diff = -want;
				moved = rt_move_bw(rt_rq, iter, &diff,
						   rt_period, false);
				if (moved)
					want += diff;
			}
			raw_spin_unlock(&iter->rt_runtime_lock);

			if (!want)
				break;
		}

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We cannot be left wanting - that would mean some runtime
		 * leaked out of the system.
		 */
		BUG_ON(want);
balanced:
		/*
		 * Disable all the borrow logic by pretending we have inf
		 * runtime - in which case borrowing doesn't make sense.
		 */
		rt_rq->rt_runtime = RUNTIME_INF;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

static void disable_runtime(struct rq *rq)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	__disable_runtime(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

static void __enable_runtime(struct rq *rq)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	if (unlikely(!scheduler_running))
		return;

	/*
	 * Reset free bandwidth on rq before runtime migration can
	 * restart (ie. before rt_runtime's become != RUNTIME_INF).
	 */
	rq->rt.rt_edf_tree.rt_free_bw = rt_free_bandwidth() -
					rt_used_bandwidth();

	/*
	 * Reset each runqueue's bandwidth settings
	 */
	for_each_rt_rq(rt_rq, iter, rq) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		raw_spin_lock(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime = rt_b->rt_runtime;
		rt_rq->rt_time = 0;
		rt_rq->rt_throttled = 0;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock(&rt_b->rt_runtime_lock);
	}
}

static void enable_runtime(struct rq *rq)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	__enable_runtime(rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

int update_runtime(struct notifier_block *nfb, unsigned long action, void *hcpu)
{
	int cpu = (int)(long)hcpu;

	switch (action) {
	case CPU_DOWN_PREPARE:
	case CPU_DOWN_PREPARE_FROZEN:
		disable_runtime(cpu_rq(cpu));
		return NOTIFY_OK;

	case CPU_DOWN_FAILED:
	case CPU_DOWN_FAILED_FROZEN:
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		enable_runtime(cpu_rq(cpu));
		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}

static inline void balance_runtime(struct rt_rq *rt_rq)
{
	if (!sched_feat(RT_RUNTIME_SHARE))
		return;

	raw_spin_unlock(&rt_rq->rt_runtime_lock);
	do_balance_runtime(rt_rq);
	raw_spin_lock(&rt_rq->rt_runtime_lock);
}

#else /* !CONFIG_SMP */
static inline void balance_runtime(struct rt_rq *rt_rq) {}

void rt_reset_runtime(void)
{
	struct rq *rq = this_rq();
	struct rt_bandwidth *rt_b;
	struct rt_rq *rt_rq;
	rt_rq_iter_t iter;

	for_each_rt_rq(rt_rq, iter, rq) {
		rt_b = sched_rt_bandwidth(rt_rq);

		raw_spin_lock_irq(&rt_b->rt_runtime_lock);
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_runtime= rt_b->rt_runtime;
		rt_rq->rt_period = rt_b->rt_period;
		rt_rq->rt_time = 0;
		rt_rq->rt_bw = to_ratio(rt_b->rt_period, rt_b->rt_runtime);
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
		raw_spin_unlock_irq(&rt_b->rt_runtime_lock);
	}
}
#endif /* CONFIG_SMP */

static inline bool rt_rq_needs_recharge(struct rt_rq *rt_rq)
{
	if (rt_rq->rt_time)
		return 1;

	BUG_ON(rt_rq->rt_nr_running && !rt_rq_on_rq(rt_rq));

	return 0;
}

static bool rt_rq_recharge(struct rt_rq *rt_rq, int overrun)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	u64 runtime;
	bool idle = true;

	/*
	 * !rt_rq->rt_time && rt_rq->rt_throttled can happen if rt_rq
	 * changed its parameters, we update them lazily here, to avoid
	 * touching the timer from the configuration code.
	 */
	if (rt_rq->rt_time || rt_rq->rt_throttled) {
		runtime = rt_rq->rt_runtime;
		rt_rq->rt_time -= min(rt_rq->rt_time, overrun * runtime);
		rt_rq->rt_deadline += overrun * ktime_to_ns(rt_rq->rt_period);
		if (rt_rq->rt_time || rt_rq->rt_nr_running)
			idle = false;

		if (rt_rq->rt_throttled && rt_rq->rt_time < runtime) {
			/* Un-throttle (even if we were boosted). */
			rt_rq->rt_throttled = 0;
			sched_rt_rq_enqueue(rt_rq);
			/*
			 * Force a clock update if the CPU was idle,
			 * lest wakeup -> unthrottle time accumulate.
			 */
			if (rt_rq->rt_nr_running && rq->curr == rq->idle)
				rq->skip_clock_update = -1;
		} else if (!rt_rq_throttled(rt_rq)) {
			/* The deadline changed, (re-)queue rt_rq. */
			sched_rt_deadline_updated(rt_rq);
		}
	} else if (rt_rq->rt_nr_running)
		idle = false;

	return idle && !rt_rq_needs_recharge(rt_rq);
}

static bool do_sched_rt_period_timer(struct rt_rq *rt_rq, int overrun)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	bool idle;

	if (!rt_bandwidth_enabled() || rt_rq->rt_runtime == RUNTIME_INF)
		return true;

	raw_spin_lock(&rq->lock);
	raw_spin_lock(&rt_rq->rt_runtime_lock);

	if (rt_rq->rt_nr_running)
		balance_runtime(rt_rq);
	idle = rt_rq_recharge(rt_rq, overrun);

	raw_spin_unlock(&rt_rq->rt_runtime_lock);
	raw_spin_unlock(&rq->lock);

	return idle;
}

static enum hrtimer_restart sched_rt_period_timer(struct hrtimer *hrtimer)
{
	struct rt_rq *rt_rq = container_of(hrtimer, struct rt_rq,
						rt_period_timer);
	int overrun;
	bool idle = false;

	if (rt_rq->rt_needs_resync) {
		/*
		 * Resync the period timer with the actual deadline;
		 * we want to be careful and check again for overruns
		 * and recharges.
		 * Note that if rt_needs_resync is set we don't need
		 * to recharge according to the old deadline, as the
		 * deadline update path has reset deadline and rt_time
		 * for us. In the common case we don't expect overruns,
		 * as the new deadline should be in the future, so we
		 * need to restart the timer only if the rt_rq is already
		 * throttled; if this is not the case we handle the overruns
		 * as usual, and they'll give us a new value for idle.
		 */
		idle = !rt_rq->rt_throttled;
		rt_rq->rt_needs_resync = false;
		rt_period_set_expires(rt_rq, true);
	}

	for (;;) {
		overrun = hrtimer_forward_now(hrtimer, rt_rq->rt_period);

		if (overrun) {
			/* Recharge as if we had expired overrun times. */
			idle = do_sched_rt_period_timer(rt_rq, overrun);

		} else {
			/*
			 * No overruns, even after the eventual resync,
			 * the timer is either ready or won't need to be
			 * restarted.
			 */
			break;
		}
	}

	BUG_ON(rt_rq->rt_nr_running && !rt_rq_on_rq(rt_rq) && idle);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void start_rt_period_timer(struct rt_rq *rt_rq)
{
	ktime_t soft, hard;
	unsigned long range;
	int overrun;

	rt_period_set_expires(rt_rq, false);

	for (;;) {
		/* Timer started, we'll get our recharge. */
		if (hrtimer_active(&rt_rq->rt_period_timer))
			break;

		/* Make sure dline is in the future when the timer starts. */
		overrun = hrtimer_forward_now(&rt_rq->rt_period_timer,
					      rt_rq->rt_period);

		/* Update deadline and handle recharges in case of overrun. */
		rt_compensate_overruns(rt_rq, overrun);

		/* Avoid unnecessary timer expirations. */
		if (!rt_rq_needs_recharge(rt_rq))
			break;

		/* Try to program the timer. */
		soft = hrtimer_get_softexpires(&rt_rq->rt_period_timer);
		hard = hrtimer_get_expires(&rt_rq->rt_period_timer);
		range = ktime_to_ns(ktime_sub(hard, soft));
		__hrtimer_start_range_ns(&rt_rq->rt_period_timer, soft,
					 range, HRTIMER_MODE_ABS, 0);
	}

	BUG_ON(!hrtimer_active(&rt_rq->rt_period_timer) &&
		rt_rq->rt_nr_running && !rt_rq_on_rq(rt_rq));
}

static inline int rt_se_prio(struct sched_rt_entity *rt_se)
{

	return rt_task_of(rt_se)->prio;
}

#ifdef CONFIG_RT_GROUP_STATS
static inline void sched_rt_stats_exceeded(struct rt_rq *rt_rq)
{
	rt_rq->rt_stats_exceeded++;
}

static inline void sched_rt_stats_reset(struct rt_rq *rt_rq)
{
	rt_rq->rt_stats_reset++;
}
#else
static inline void sched_rt_stats_exceeded(struct rt_rq *rt_rq)
{
}

static inline void sched_rt_stats_reset(struct rt_rq *rt_rq)
{
}
#endif

static int sched_rt_runtime_exceeded(struct rt_rq *rt_rq)
{
	u64 runtime = sched_rt_runtime(rt_rq);

	if (rt_rq->rt_throttled)
		return rt_rq_throttled(rt_rq);

	if (runtime >= sched_rt_period(rt_rq))
		return 0;

	runtime = sched_rt_runtime(rt_rq);
	if (runtime == RUNTIME_INF)
		return 0;

	if (rt_rq->rt_time < runtime)
		return 0;

	balance_runtime(rt_rq);
	if (rt_rq->rt_time >= rt_rq->rt_runtime) {
		struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);

		/*
		 * Don't actually throttle groups that have no runtime assigned
		 * but accrue some time due to boosting.
		 */
		if (likely(rt_b->rt_runtime)) {
			rt_rq->rt_throttled = 1;
			sched_rt_stats_exceeded(rt_rq);
			start_rt_period_timer(rt_rq);
		} else {
			static bool once = false;
			/*
			 * In case we did anyway, make it go away,
			 * replenishment is a joke, since it will replenish us
			 * with exactly 0 ns.
			 */
			rt_rq->rt_time = 0;

			if (!once) {
				once = true;
				printk_sched("sched: RT throttling bypassed\n");
			}
		}

		if (rt_rq_throttled(rt_rq)) {
			BUG_ON(!hrtimer_active(&rt_rq->rt_period_timer));
			sched_rt_rq_dequeue(rt_rq);
			return 1;
		}
	}

	return 0;
}

/*
 * Update the current task's runtime statistics. Skip current tasks that
 * are not in our scheduling class.
 */
static void update_curr_rt(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_rt_entity *rt_se = &curr->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	u64 delta_exec;

	if (curr->sched_class != &rt_sched_class)
		return;

	delta_exec = rq->clock_task - curr->se.exec_start;
	if (unlikely((s64)delta_exec < 0))
		delta_exec = 0;

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = rq->clock_task;
	cpuacct_charge(curr, delta_exec);

	sched_rt_avg_update(rq, delta_exec);

	if (!rt_bandwidth_enabled())
		return;

	if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
		raw_spin_lock(&rt_rq->rt_runtime_lock);
		rt_rq->rt_time += delta_exec;
		if (sched_rt_runtime_exceeded(rt_rq))
			resched_task(curr);
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
}

#if defined CONFIG_SMP

static void
inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (rq->online && prio < prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, prio);
}

static void
dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (rq->online && rq->rt.highest_prio.curr != prev_prio)
		cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

#else /* CONFIG_SMP */

static inline
void inc_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}
static inline
void dec_rt_prio_smp(struct rt_rq *rt_rq, int prio, int prev_prio) {}

#endif /* CONFIG_SMP */

#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
static void
inc_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio;
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (prio < prev_prio)
		rt_rq->highest_prio = prio;

	prev_prio = rq->rt.highest_prio.curr;

	if (prio < prev_prio)
		rq->rt.highest_prio.curr = prio;

	inc_rt_prio_smp(rt_rq, prio, prev_prio);
}

static void
dec_rt_prio(struct rt_rq *rt_rq, int prio)
{
	int prev_prio = rt_rq->highest_prio;
	struct rq *rq = rq_of_rt_rq(rt_rq);

	if (rt_rq->rt_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct rt_prio_array *array = &rt_rq->active;

			rt_rq->highest_prio =
				sched_find_first_bit(array->bitmap);
		}

	} else
		rt_rq->highest_prio = MAX_RT_PRIO;

	prev_prio = rq->rt.highest_prio.curr;
	if (prio == prev_prio) {
		prio = MAX_RT_PRIO;

		for_each_leaf_rt_rq(rt_rq, rq) {
			if (rt_rq->highest_prio < prio)
				prio = rt_rq->highest_prio;
		}
		rq->rt.highest_prio.curr = prio;
	}

	dec_rt_prio_smp(rt_rq, prio, prev_prio);
}

#else

static inline void inc_rt_prio(struct rt_rq *rt_rq, int prio) {}
static inline void dec_rt_prio(struct rt_rq *rt_rq, int prio) {}

#endif /* CONFIG_SMP || CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_RT_GROUP_SCHED

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted++;
}

static void
dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	if (rt_se_boosted(rt_se))
		rt_rq->rt_nr_boosted--;

	WARN_ON(!rt_rq->rt_nr_running && rt_rq->rt_nr_boosted);
}

static inline int rt_rq_prio(struct rt_rq *rt_rq)
{
	return rt_rq->highest_prio;
}

static inline int rt_rq_before(struct rt_rq *a, struct rt_rq *b)
{
	/*
	 * Schedule by priority if:
	 * - both a and b are boosted;
	 * - throttling is disabled system-wide.
	 */
	if ((a->rt_nr_boosted && b->rt_nr_boosted) ||
	    global_rt_runtime() == RUNTIME_INF)
		return rt_rq_prio(a) < rt_rq_prio(b);

	/* Only a is boosted, choose it. */
	if (a->rt_nr_boosted)
		return 1;

	/* Only b is boosted, choose it. */
	if (b->rt_nr_boosted)
		return 0;

	/* Order by deadline. */
	return rt_time_before(a->rt_deadline, b->rt_deadline);
}

static void __enqueue_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	struct rt_edf_tree *rt_tree = &rq->rt.rt_edf_tree;
	struct rb_node **link = &rt_tree->rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct rt_rq *entry;
	int leftmost = 1;

	BUG_ON(rt_rq_on_rq(rt_rq));

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct rt_rq, rb_node);
		if (rt_rq_before(rt_rq, entry))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		rt_tree->rb_leftmost = &rt_rq->rb_node;

	rb_link_node(&rt_rq->rb_node, parent, link);
	rb_insert_color(&rt_rq->rb_node, &rt_tree->rb_root);
}

static void __dequeue_rt_rq(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	struct rt_edf_tree *rt_tree = &rq->rt.rt_edf_tree;

	BUG_ON(!rt_rq_on_rq(rt_rq));

	if (rt_tree->rb_leftmost == &rt_rq->rb_node)
		rt_tree->rb_leftmost = rb_next(&rt_rq->rb_node);

	rb_erase(&rt_rq->rb_node, &rt_tree->rb_root);
	RB_CLEAR_NODE(&rt_rq->rb_node);
}

static void rt_rq_update_deadline(struct rt_rq *rt_rq)
{
	struct rq *rq = rq_of_rt_rq(rt_rq);
	u64 runtime, period, left, right;

	raw_spin_lock(&rt_rq->rt_runtime_lock);

	runtime = rt_rq->rt_runtime;
	period = ktime_to_ns(rt_rq->rt_period);

	/*
	 * Update the deadline to the current time only if:
	 * - it is in the past;
	 * - using it would lead to a timeframe during which the
	 *   group would exceed ist allocated bandwidth.
	 *
	 * For the second condition to hold, we check that in the
	 * time left before the deadline, using the residual budget,
	 * the group would exceed its runtime / period share.
	 * In formula:
	 *   (runtime - rt_time) / (deadline - rq->clock) >= runtime / period
	 *
	 * left and right are the two sides of the equation, after a bit
	 * of shuffling to use multiplications instead of divisions; the
	 * goto are there to avoid the multiplications when not necessary.
	 */
	if (rt_time_before(rt_rq->rt_deadline, rq->clock))
		goto update;

	WARN_ON_ONCE(rt_rq->rt_time > runtime);

	left = period * (runtime - rt_rq->rt_time);
	right = (rt_rq->rt_deadline - rq->clock) * rt_rq->rt_runtime;

	if (rt_time_before(right, left)) {
update:
		rt_rq->rt_deadline = rq->clock + period;
		rt_rq->rt_time -= min(runtime, rt_rq->rt_time);

		sched_rt_stats_reset(rt_rq);

		/*
		* Be sure to return a runqueue that can execute, if it
		* was boosted and consumed too much runtime; postpone
		* the deadline accordingly.
		*/
		BUG_ON(!runtime);
		while (rt_rq->rt_time > runtime) {
			rt_rq->rt_deadline += period;
			rt_rq->rt_time -= runtime;
		}

		/* Let the timer handler know that it needs to resync. */
		if (hrtimer_active(&rt_rq->rt_period_timer))
			rt_rq->rt_needs_resync = true;
	}
	raw_spin_unlock(&rt_rq->rt_runtime_lock);
}

static inline int rt_rq_boosted(struct rt_rq *rt_rq)
{
	if (!rt_rq->rt_nr_boosted)
		return MAX_RT_PRIO;

	return rt_rq_prio(rt_rq);
}

static void enqueue_rt_rq(struct rt_rq *rt_rq, int old_boosted)
{
	int on_rq = rt_rq_on_rq(rt_rq);

	BUG_ON(!rt_rq->rt_nr_running);
	BUG_ON(on_rq && rt_rq_throttled(rt_rq));

	if (on_rq) {
		if (old_boosted != rt_rq_boosted(rt_rq)) {
			/* Boosted priority/state change: requeue rt_rq. */
			__dequeue_rt_rq(rt_rq);
		} else {
			/* Already queued properly. */
			return;
		}
	}

	if (rt_rq_throttled(rt_rq))
		return;

	if (!rt_rq->rt_nr_boosted) {
		rt_rq_update_deadline(rt_rq);
		on_rq = rt_rq_on_rq(rt_rq);
	}

	if (!on_rq)
		__enqueue_rt_rq(rt_rq);
}

static void dequeue_rt_rq(struct rt_rq *rt_rq, int old_boosted)
{
	int on_rq = rt_rq_on_rq(rt_rq);

	/*
	 * Here we do not expect throttled rt_rq's to be in the
	 * EDF tree; note that when they exceed their assigned budget
	 * they are dequeued via sched_rt_rq_dequeue().
	 */
	BUG_ON(on_rq && rt_rq_throttled(rt_rq));

	if (on_rq && (!rt_rq->rt_nr_running ||
	    old_boosted != rt_rq_boosted(rt_rq))) {
		/*
		 * Dequeue the rt_rq either if it has no more tasks or
		 * its boosted priority/status changed and it needs to
		 * be requeued.
		 */
		__dequeue_rt_rq(rt_rq);
		on_rq = 0;
	}

	/* If we do not need to requeue the rt_rq, just return. */
	if (!rt_rq->rt_nr_running || rt_rq_throttled(rt_rq))
		return;

	/* Reposition rt_rq. */
	if (!on_rq)
		__enqueue_rt_rq(rt_rq);
}

#else /* CONFIG_RT_GROUP_SCHED */

static void
inc_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	raw_spin_lock(&rt_rq->rt_runtime_lock);
	start_rt_period_timer(rt_rq);
	raw_spin_unlock(&rt_rq->rt_runtime_lock);
}

static inline
void dec_rt_group(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq) {}

static inline int rt_rq_boosted(struct rt_rq *rt_rq)
{
	return MAX_RT_PRIO;
}

static inline int rt_rq_before(struct rt_rq *a, struct rt_rq *b)
{
	BUG();

	return 0;
}

static inline void enqueue_rt_rq(struct rt_rq *rt_rq, int old_boosted) {}
static inline void dequeue_rt_rq(struct rt_rq *rt_rq, int old_boosted) {}

#endif /* CONFIG_RT_GROUP_SCHED */

static inline
void inc_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running++;

	inc_rt_migration(rt_se, rt_rq);
	inc_rt_prio(rt_rq, prio);
	inc_rt_group(rt_se, rt_rq);
}

static inline
void dec_rt_tasks(struct sched_rt_entity *rt_se, struct rt_rq *rt_rq)
{
	int prio = rt_se_prio(rt_se);

	WARN_ON(!rt_rq->rt_nr_running);
	WARN_ON(!rt_prio(prio));
	rt_rq->rt_nr_running--;

	dec_rt_migration(rt_se, rt_rq);
	dec_rt_prio(rt_rq, prio);
	dec_rt_group(rt_se, rt_rq);
}

static void enqueue_rt_entity(struct sched_rt_entity *rt_se, bool head)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	struct list_head *queue = array->queue + rt_se_prio(rt_se);
	int boosted = rt_rq_boosted(rt_rq);

	if (!rt_rq->rt_nr_running)
		list_add_leaf_rt_rq(rt_rq);

	if (head)
		list_add(&rt_se->run_list, queue);
	else
		list_add_tail(&rt_se->run_list, queue);
	__set_bit(rt_se_prio(rt_se), array->bitmap);

	inc_rt_tasks(rt_se, rt_rq);

	enqueue_rt_rq(rt_rq, boosted);
}

static void dequeue_rt_entity(struct sched_rt_entity *rt_se)
{
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);
	struct rt_prio_array *array = &rt_rq->active;
	int boosted = rt_rq_boosted(rt_rq);

	list_del_init(&rt_se->run_list);
	if (list_empty(array->queue + rt_se_prio(rt_se)))
		__clear_bit(rt_se_prio(rt_se), array->bitmap);

	dec_rt_tasks(rt_se, rt_rq);
	dequeue_rt_rq(rt_rq, boosted);

	if (!rt_rq->rt_nr_running)
		list_del_leaf_rt_rq(rt_rq);
}

/*
 * Adding/removing a task to/from a priority array:
 */
static void
enqueue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	if (flags & ENQUEUE_WAKEUP)
		rt_se->timeout = 0;

	enqueue_rt_entity(rt_se, flags & ENQUEUE_HEAD);

	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);

	inc_nr_running(rq);
}

static void dequeue_task_rt(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_rt_entity *rt_se = &p->rt;

	update_curr_rt(rq);
	dequeue_rt_entity(rt_se);

	dequeue_pushable_task(rq, p);

	dec_nr_running(rq);
}

/*
 * Put task to the head or the end of the run list without the overhead of
 * dequeue followed by enqueue.
 */
static void
requeue_rt_entity(struct rt_rq *rt_rq, struct sched_rt_entity *rt_se, int head)
{
	if (on_rt_rq(rt_se)) {
		struct rt_prio_array *array = &rt_rq->active;
		struct list_head *queue = array->queue + rt_se_prio(rt_se);

		if (head)
			list_move(&rt_se->run_list, queue);
		else
			list_move_tail(&rt_se->run_list, queue);
	}
}

static void requeue_task_rt(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_rt_entity *rt_se = &p->rt;
	struct rt_rq *rt_rq = rt_rq_of_se(rt_se);

	requeue_rt_entity(rt_rq, rt_se, head);
}

static void yield_task_rt(struct rq *rq)
{
	requeue_task_rt(rq, rq->curr, 0);
}

#ifdef CONFIG_SMP
static int find_lowest_rq(struct task_struct *task);

static int
select_task_rq_rt(struct task_struct *p, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;
	int cpu;

	cpu = task_cpu(p);

	if (p->nr_cpus_allowed == 1)
		goto out;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = ACCESS_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an RT task, then
	 * try to see if we can wake this RT task up on another
	 * runqueue. Otherwise simply start this RT task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 */
	if (curr && unlikely(rt_task(curr)) &&
	    (curr->nr_cpus_allowed < 2 ||
	     curr->prio <= p->prio) &&
	    (p->nr_cpus_allowed > 1)) {
		int target = find_lowest_rq(p);

		if (target != -1)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}

static void check_preempt_equal_prio(struct rq *rq, struct task_struct *p)
{
	if (rq->curr->nr_cpus_allowed == 1)
		return;

	if (p->nr_cpus_allowed != 1
	    && cpupri_find(&rq->rd->cpupri, p, NULL))
		return;

	if (!cpupri_find(&rq->rd->cpupri, rq->curr, NULL))
		return;

	/*
	 * There appears to be other cpus that can accept
	 * current and none to run 'p', so lets reschedule
	 * to try and push current away:
	 */
	requeue_task_rt(rq, p, 1);
	resched_task(rq->curr);
}

#endif /* CONFIG_SMP */

static inline int check_preempt_rt_rq(struct task_struct *curr,
				      struct task_struct *p)
{
	struct rt_rq *rt_rq, *cur_rq;

	cur_rq = rt_rq_of_se(&curr->rt);
	rt_rq = rt_rq_of_se(&p->rt);

	if (rt_rq == cur_rq)
		return p->prio < curr->prio;

	return rt_rq_before(rt_rq, cur_rq);
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_curr_rt(struct rq *rq, struct task_struct *p, int flags)
{
	if (check_preempt_rt_rq(rq->curr, p)) {
		resched_task(rq->curr);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If:
	 *
	 * - the newly woken task is of equal priority to the current task
	 * - the newly woken task is non-migratable while current is migratable
	 * - current will be preempted on the next reschedule
	 *
	 * we should check to see if current can readily move to a different
	 * cpu.  If so, we will reschedule to allow the push logic to try
	 * to move current somewhere else, making room for our non-migratable
	 * task.
	 */
	if (p->prio == rq->curr->prio && !test_tsk_need_resched(rq->curr))
		check_preempt_equal_prio(rq, p);
#endif
}

static struct sched_rt_entity *pick_next_rt_entity(struct rq *rq)
{
	struct rt_rq *rt_rq;
	struct rt_prio_array *array;
	struct sched_rt_entity *next;
	struct list_head *queue;
	int idx;

	rt_rq = pick_next_rt_rq(rq);
	if (!rt_rq)
		return NULL;

	array = &rt_rq->active;
	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_RT_PRIO);

	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_rt_entity, run_list);

	return next;
}

static struct task_struct *_pick_next_task_rt(struct rq *rq)
{
	struct sched_rt_entity *rt_se;
	struct task_struct *p;

	rt_se = pick_next_rt_entity(rq);
	if (!rt_se)
		return NULL;

	p = rt_task_of(rt_se);
	p->se.exec_start = rq->clock_task;

	return p;
}

static struct task_struct *pick_next_task_rt(struct rq *rq)
{
	struct task_struct *p = _pick_next_task_rt(rq);

	/* The running task is never eligible for pushing */
	if (p)
		dequeue_pushable_task(rq, p);

#ifdef CONFIG_SMP
	/*
	 * We detect this state here so that we can avoid taking the RQ
	 * lock again later if there is no need to push
	 */
	rq->post_schedule = has_pushable_tasks(rq);
#endif

	return p;
}

static void put_prev_task_rt(struct rq *rq, struct task_struct *p)
{
	update_curr_rt(rq);

	/*
	 * The previous task needs to be made eligible for pushing
	 * if it is still active
	 */
	if (on_rt_rq(&p->rt) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define RT_MAX_TRIES 3

static int pick_rt_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    (cpu < 0 || cpumask_test_cpu(cpu, tsk_cpus_allowed(p))) &&
	    (p->nr_cpus_allowed > 1))
		return 1;
	return 0;
}

/* Return the second highest RT task, NULL otherwise */
static struct task_struct *pick_next_highest_task_rt(struct rq *rq, int cpu)
{
	struct task_struct *next = NULL;
	struct sched_rt_entity *rt_se;
	struct rt_prio_array *array;
	struct rt_rq *rt_rq;
	int idx;

	for_each_leaf_rt_rq(rt_rq, rq) {
		array = &rt_rq->active;
		idx = sched_find_first_bit(array->bitmap);
next_idx:
		if (idx >= MAX_RT_PRIO)
			continue;
		if (next && next->prio <= idx)
			continue;
		list_for_each_entry(rt_se, array->queue + idx, run_list) {
			struct task_struct *p;

			p = rt_task_of(rt_se);
			if (pick_rt_task(rq, p, cpu)) {
				next = p;
				break;
			}
		}
		if (!next) {
			idx = find_next_bit(array->bitmap, MAX_RT_PRIO, idx+1);
			goto next_idx;
		}
	}

	return next;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask);

static int find_lowest_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = __get_cpu_var(local_cpu_mask);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (task->nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	if (!cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask))
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of cpus representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last cpu that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which cpu is logically closest to our hot cache data.
	 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_first_and(lowest_mask,
						     sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;
	return -1;
}

/* Will lock the rq it finds */
static struct rq *find_lock_lowest_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *lowest_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < RT_MAX_TRIES; tries++) {
		cpu = find_lowest_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		lowest_rq = cpu_rq(cpu);

		/* if the prio of this runqueue changed, try again */
		if (double_lock_balance(rq, lowest_rq)) {
			/*
			 * We had to unlock the run queue. In
			 * the mean time, task could have
			 * migrated already or had its affinity changed.
			 * Also make sure that it wasn't scheduled on its rq.
			 */
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(lowest_rq->cpu,
						       tsk_cpus_allowed(task)) ||
				     task_running(rq, task) ||
				     !task->on_rq)) {

				double_unlock_balance(rq, lowest_rq);
				lowest_rq = NULL;
				break;
			}
		}

		/* If this rq is still suitable use it. */
		if (lowest_rq->rt.highest_prio.curr > task->prio)
			break;

		/* try again */
		double_unlock_balance(rq, lowest_rq);
		lowest_rq = NULL;
	}

	return lowest_rq;
}

static struct task_struct *pick_next_pushable_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	p = plist_first_entry(&rq->rt.pushable_tasks,
			      struct task_struct, pushable_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(p->nr_cpus_allowed <= 1);

	BUG_ON(!p->on_rq);
	BUG_ON(!rt_task(p));

	return p;
}

/*
 * If the current CPU has more than one RT task, see if the non
 * running task can migrate over to a CPU that is running a task
 * of lesser priority.
 */
static int push_rt_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *lowest_rq;
	int ret = 0;

	if (!rq->rt.overloaded)
		return 0;

	next_task = pick_next_pushable_task(rq);
	if (!next_task)
		return 0;

retry:
	if (unlikely(next_task == rq->curr)) {
		WARN_ON(1);
		return 0;
	}

	/*
	 * It's possible that the next_task slipped in of
	 * higher priority than current. If that's the case
	 * just reschedule current.
	 */
	if (unlikely(next_task->prio < rq->curr->prio)) {
		resched_task(rq->curr);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* find_lock_lowest_rq locks the rq if found */
	lowest_rq = find_lock_lowest_rq(next_task, rq);
	if (!lowest_rq) {
		struct task_struct *task;
		/*
		 * find_lock_lowest_rq releases rq->lock
		 * so it is possible that next_task has migrated.
		 *
		 * We need to make sure that the task is still on the same
		 * run-queue and is also still the next task eligible for
		 * pushing.
		 */
		task = pick_next_pushable_task(rq);
		if (task_cpu(next_task) == rq->cpu && task == next_task) {
			/*
			 * The task hasn't migrated, and is still the next
			 * eligible task, but we failed to find a run-queue
			 * to push it to.  Do not retry in this case, since
			 * other cpus will pull from us when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks, just exit */
			goto out;

		/*
		 * Something has shifted, try again.
		 */
		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	set_task_cpu(next_task, lowest_rq->cpu);
	activate_task(lowest_rq, next_task, 0);
	ret = 1;

	resched_task(lowest_rq->curr);

	double_unlock_balance(rq, lowest_rq);

out:
	put_task_struct(next_task);

	return ret;
}

static void push_rt_tasks(struct rq *rq)
{
	/* push_rt_task will return true if it moved an RT */
	while (push_rt_task(rq))
		;
}

static int pull_rt_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, ret = 0, cpu;
	struct task_struct *p;
	struct rq *src_rq;

	if (likely(!rt_overloaded(this_rq)))
		return 0;

	for_each_cpu(cpu, this_rq->rd->rto_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * Don't bother taking the src_rq->lock if the next highest
		 * task is known to be lower-priority than our current task.
		 * This may look racy, but if this value is about to go
		 * logically higher, the src_rq will push this task away.
		 * And if its going logically lower, we do not care
		 */
		if (src_rq->rt.highest_prio.next >=
		    this_rq->rt.highest_prio.curr)
			continue;

		/*
		 * We can potentially drop this_rq's lock in
		 * double_lock_balance, and another CPU could
		 * alter this_rq
		 */
		double_lock_balance(this_rq, src_rq);

		/*
		 * Are there still pullable RT tasks?
		 */
		if (src_rq->rt.rt_nr_total <= 1)
			goto skip;

		p = pick_next_highest_task_rt(src_rq, this_cpu);

		/*
		 * Do we have an RT task that preempts
		 * the to-be-scheduled task?
		 */
		if (p && (p->prio < this_rq->rt.highest_prio.curr)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!p->on_rq);

			/*
			 * There's a chance that p is higher in priority
			 * than what's currently running on its cpu.
			 * This is just that p is wakeing up and hasn't
			 * had a chance to schedule. We only pull
			 * p if it is lower in priority than the
			 * current task on the run queue
			 */
			if (p->prio < src_rq->curr->prio)
				goto skip;

			ret = 1;

			deactivate_task(src_rq, p, 0);
			set_task_cpu(p, this_cpu);
			activate_task(this_rq, p, 0);
			/*
			 * We continue with the search, just in
			 * case there's an even higher prio task
			 * in another runqueue. (low likelihood
			 * but possible)
			 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	return ret;
}

static void pre_schedule_rt(struct rq *rq, struct task_struct *prev)
{
	/* Try to pull RT tasks here if we lower this rq's prio */
	if (rq->rt.highest_prio.curr > prev->prio)
		pull_rt_task(rq);
}

static void post_schedule_rt(struct rq *rq)
{
	push_rt_tasks(rq);
}

/*
 * If we are not running and we are not going to reschedule soon, we should
 * try to push tasks away now
 */
static void task_woken_rt(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    has_pushable_tasks(rq) &&
	    p->nr_cpus_allowed > 1 &&
	    (dl_task(rq->curr) || rt_task(rq->curr)) &&
	    (rq->curr->nr_cpus_allowed < 2 ||
	     rq->curr->prio <= p->prio))
		push_rt_tasks(rq);
}

static void set_cpus_allowed_rt(struct task_struct *p,
				const struct cpumask *new_mask)
{
	struct rq *rq;
	int weight;

	BUG_ON(!rt_task(p));

	if (!p->on_rq)
		return;

	weight = cpumask_weight(new_mask);

	/*
	 * Only update if the process changes its state from whether it
	 * can migrate or not.
	 */
	if ((p->nr_cpus_allowed > 1) == (weight > 1))
		return;

	rq = task_rq(p);

	/*
	 * The process used to be able to migrate OR it can now migrate
	 */
	if (weight <= 1) {
		if (!task_current(rq, p))
			dequeue_pushable_task(rq, p);
		BUG_ON(!rq->rt.rt_nr_migratory);
		rq->rt.rt_nr_migratory--;
	} else {
		if (!task_current(rq, p))
			enqueue_pushable_task(rq, p);
		rq->rt.rt_nr_migratory++;
	}

	update_rt_migration(&rq->rt);
}

/* Assumes rq->lock is held */
static void rq_online_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_set_overload(rq);

	__enable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_rt(struct rq *rq)
{
	if (rq->rt.overloaded)
		rt_clear_overload(rq);

	__disable_runtime(rq);

	cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_INVALID);
}

/*
 * When switch from the rt queue, we bring ourselves to a position
 * that we might want to pull RT tasks from other runqueues.
 */
static void switched_from_rt(struct rq *rq, struct task_struct *p)
{
	/*
	 * If there are other RT tasks then we will reschedule
	 * and the scheduling of the other RT tasks will handle
	 * the balancing. But if we are the last RT task
	 * we may need to handle the pulling of RT tasks
	 * now.
	 */
	if (p->on_rq && !rq->rt.rt_nr_total)
		pull_rt_task(rq);
}

void init_sched_rt_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask, i),
					GFP_KERNEL, cpu_to_node(i));
	}
}
#endif /* CONFIG_SMP */

/*
 * When switching a task to RT, we may overload the runqueue
 * with RT tasks. In this case we try to push them off to
 * other runqueues.
 */
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
	int check_resched = 1;

	/*
	 * If we are already running, then there's nothing
	 * that needs to be done. But if we are not running
	 * we may need to preempt the current running task.
	 * If that current running task is also an RT task
	 * then see if we can move to another run queue.
	 */
	if (p->on_rq && rq->curr != p) {
#ifdef CONFIG_SMP
		if (rq->rt.overloaded && push_rt_task(rq) &&
		    /* Don't resched if we changed runqueues */
		    rq != task_rq(p))
			check_resched = 0;
#endif /* CONFIG_SMP */
		if (check_resched && p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

/*
 * Priority of the task has changed. This may cause
 * us to initiate a push or pull.
 */
static void
prio_changed_rt(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!p->on_rq)
		return;

	if (rq->curr == p) {
#ifdef CONFIG_SMP
		/*
		 * If our priority decreases while running, we
		 * may need to pull tasks to this runqueue.
		 */
		if (oldprio < p->prio)
			pull_rt_task(rq);
		/*
		 * If there's a higher priority task waiting to run
		 * then reschedule. Note, the above pull_rt_task
		 * can release the rq lock and p could migrate.
		 * Only reschedule if p is still on the same runqueue.
		 */
		if (p->prio > rq->rt.highest_prio.curr && rq->curr == p)
			resched_task(p);
#else
		/* For UP simply resched on drop of prio */
		if (oldprio < p->prio)
			resched_task(p);
#endif /* CONFIG_SMP */
	} else {
		/*
		 * This task is not running, but if it is
		 * greater than the current running task
		 * then reschedule.
		 */
		if (p->prio < rq->curr->prio)
			resched_task(rq->curr);
	}
}

static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		p->rt.timeout++;
		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next)
			p->cputime_expires.sched_exp = p->se.sum_exec_runtime;
	}
}

static void task_tick_rt(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_rt(rq);

	watchdog(rq, p);

	/*
	 * RR tasks need a special form of timeslice management.
	 * FIFO tasks have no timeslices.
	 */
	if (p->policy != SCHED_RR)
		return;

	if (--p->rt.time_slice)
		return;

	p->rt.time_slice = RR_TIMESLICE;

	/*
	 * Requeue to the end of queue if we are the only element
	 * on the queue
	 */
	if (p->rt.run_list.prev != p->rt.run_list.next) {
			requeue_task_rt(rq, p, 0);
			set_tsk_need_resched(p);
	}
}

static void set_curr_task_rt(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq->clock_task;

	/* The running task is never eligible for pushing */
	dequeue_pushable_task(rq, p);
}

static unsigned int get_rr_interval_rt(struct rq *rq, struct task_struct *task)
{
	/*
	 * Time slice is 0 for SCHED_FIFO tasks
	 */
	if (task->policy == SCHED_RR)
		return RR_TIMESLICE;
	else
		return 0;
}

const struct sched_class rt_sched_class = {
	.next			= &fair_sched_class,
	.enqueue_task		= enqueue_task_rt,
	.dequeue_task		= dequeue_task_rt,
	.yield_task		= yield_task_rt,

	.check_preempt_curr	= check_preempt_curr_rt,

	.pick_next_task		= pick_next_task_rt,
	.put_prev_task		= put_prev_task_rt,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_rt,

	.set_cpus_allowed       = set_cpus_allowed_rt,
	.rq_online              = rq_online_rt,
	.rq_offline             = rq_offline_rt,
	.pre_schedule		= pre_schedule_rt,
	.post_schedule		= post_schedule_rt,
	.task_woken		= task_woken_rt,
	.switched_from		= switched_from_rt,
#endif

	.set_curr_task          = set_curr_task_rt,
	.task_tick		= task_tick_rt,

	.get_rr_interval	= get_rr_interval_rt,

	.prio_changed		= prio_changed_rt,
	.switched_to		= switched_to_rt,
};

#ifdef CONFIG_SCHED_DEBUG
extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);

void print_rt_stats(struct seq_file *m, int cpu)
{
	rt_rq_iter_t iter;
	struct rt_rq *rt_rq;

	rcu_read_lock();
	for_each_rt_rq(rt_rq, iter, cpu_rq(cpu))
		print_rt_rq(m, cpu, rt_rq);
	rcu_read_unlock();
}
#endif /* CONFIG_SCHED_DEBUG */
