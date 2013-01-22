/*
 *  kernel/sched/cpudeadline.c
 *
 *  Global CPU deadline management
 *
 *  Author: Juri Lelli <j.lelli@sssup.it>
 *  Author: Fabio Falzoi <fabio.falzoi@alice.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <linux/spinlock.h>
#include "cpudeadline.h"

static inline void update_cache_slow(struct cpudl *cp)
{
	int best_cpu = NO_CPU_DL;
	u64 best_dl = NO_CACHED_DL;
	u64 current_dl;
	int i;
	
	if(!cpumask_full(cp->free_cpus))
		for_each_cpu_not(i, cp->free_cpus) {
			current_dl = (u64)atomic64_read(&cp->current_dl[i]);
			if(current_dl == NO_CACHED_DL)
				continue;
			if(best_dl == NO_CACHED_DL ||
				cp->cmp_dl(best_dl, current_dl)) {
				best_dl = current_dl;
				best_cpu = i;
			}
		}

	/*
	 * Implicit memory barrier provided by the next
	 * unlock operation.
	 */
	atomic_set(&cp->cached_cpu, best_cpu);
}

/*
 * cpudl_find - find the best CPU in the system
 * @cp: the cpudl context
 * @p: the task
 * @later_mask: a mask to fill in with the selected CPUs (or NULL)
 *
 * Returns: int - best CPU to/from migrate the task
 */
int cpudl_find(struct cpudl *cp, struct task_struct *p,
	       struct cpumask *later_mask)
{
	int now_cached_cpu = NO_CACHED_CPU;
	u64 now_cached_dl;
	unsigned long flags;
	int best_cpu = -1;
	const struct sched_dl_entity *dl_se;

	if (later_mask && cpumask_and(later_mask, cp->free_cpus,
			&p->cpus_allowed) && cpumask_and(later_mask,
			later_mask, cpu_active_mask))
		return cpumask_any(later_mask);
	/*
	 * Paired with is_valid path inside cpudl_set
	 */
	smp_rmb();
	
	while(true) {
		now_cached_cpu = atomic_read(&cp->cached_cpu);

		/*
		 * There are no CPUs with dl tasks enqueued
 		 */
		if (now_cached_cpu == NO_CPU_DL)
 			return -1;

 		/*
		 * Should the cache be updated through the slow-path?
 		 */

		if (now_cached_cpu == NO_CACHED_CPU) {
			if (raw_spin_trylock_irqsave(&cp->lock, flags)) {
				update_cache_slow(cp);
				raw_spin_unlock_irqrestore(&cp->lock, flags);
			}
			/*
			 * Someone else is probably updating the cache.
			 * Just let it do its job and see if the cache
			 * is updated in the next round.
			 */
			continue;
		}
		break;
	}

	/* 
	 * cpudl_find has been called on behalf of a pull.
	 * We don't care about cp->current_dl[now_cached_cpu]
	 * value.
	 */
	if(!p)
		return now_cached_cpu;

	/*
	 * Otherwise, if cpudl_find has been called on behalf of
	 * a push, we must check the cpus_allowed mask and the deadline.
	 *
	 * Pairing with a slow path on cpudl_set (lock/unlock) or with
	 * the cmpxchg implicit barriers.
	 */
	smp_rmb();
	now_cached_dl = (u64)atomic64_read(&cp->current_dl[now_cached_cpu]);

	/*
	 * A parallel operation may have changed the deadline value of
	 * now_cached_cpu. Give up.
	 */
	if(now_cached_dl == NO_CACHED_DL)
		return -1;
	
	dl_se = &p->dl;
	if(cpumask_test_cpu(now_cached_cpu, &p->cpus_allowed) && 
		cp->cmp_dl(dl_se->deadline, now_cached_dl)) {
		best_cpu = now_cached_cpu;
		if(later_mask)
			cpumask_set_cpu(best_cpu, later_mask);
 	}

	return best_cpu;
}

/*
 * cpudl_set - update the cpudl fast-cache
 * @cp: the cpudl fast-cache context
 * @cpu: the target cpu
 * @dl: the new earliest deadline for this cpu
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid)
{
 	int now_cached_cpu;
	u64 now_cached_dl;
	bool updated = false;
	unsigned long flags;

 	/*
 	 * We have to:
 	 *   - update the current_dl[cpu] field;
	 *   - mark cpu as busy;
	 *   - check if the cache needs an update.
 	 */
	if (is_valid) {
		atomic64_set(&cp->current_dl[cpu], dl);
		/*
		 * Deadline of the new curr must be visible
		 * before considering that cpu busy.
		 * Paired with cpudl_find smp_rmb() inside
		 * the while(true) loop.
		 */
		smp_mb__before_clear_bit();
		cpumask_clear_cpu(cpu, cp->free_cpus);

		while (1) {
			now_cached_cpu = atomic_read(&cp->cached_cpu);
			
			/*
			 * Fast-path: cache valid and pointing to someone
			 * else than us, or it has just been updated.
			 */
			if (now_cached_cpu != NO_CACHED_CPU && 
			    (now_cached_cpu != cpu || updated)) {
			    	/*
			 	 * Pairing with a slow path on cpudl_set
			 	 * (lock/unlock) or with the cmpxchg implicit
			 	 * barriers.
			 	 */
				smp_rmb();
				now_cached_dl = (u64)atomic64_read(
					&cp->current_dl[now_cached_cpu]);
			} else {
				/*
				 * Slow-path: update the cache grabbing locks.
				 */
				if (raw_spin_trylock_irqsave(&cp->lock, flags)) {
					update_cache_slow(cp);
					raw_spin_unlock_irqrestore(&cp->lock,
								   flags);
					updated = true;
				}
				/*
				 * Someone else is probably updating the cache.
				 * Just let it do its job and see if the cache
				 * is updated in the next round.
				 */
				continue;
			}
			
			if ((now_cached_cpu != NO_CPU_DL &&
			     now_cached_dl != NO_CACHED_DL &&
			     cp->cmp_dl(dl, now_cached_dl)) ||
				atomic_cmpxchg(&cp->cached_cpu,
						now_cached_cpu, cpu) == cpu) {
				/*
				 * cmpxchg must be visible before proceeding further,
				 * but it implies a barrier.
				 */	
 				break;
			}
		}
	} else {
		/*
		 * Dual of the operation above. In this case we set the
		 * deadline to an invalid value only after having signaled
		 * the corresponding cpu as free (and we need a barrier
		 * among this operation). Otherwise another cpu can see
		 * a cpu as busy, but with an invalid deadline.
		 *
		 */
		cpumask_set_cpu(cpu, cp->free_cpus);
		smp_wmb();
		atomic64_set(&cp->current_dl[cpu], NO_CACHED_DL);

 		while (1) {
			now_cached_cpu = atomic_read(&cp->cached_cpu);
			/*
			 * Follow the slow-path if the cache is invalid or
			 * we were the cpu pointed by it.
			 */
			if((now_cached_cpu == NO_CACHED_CPU ||
			    now_cached_cpu == cpu)) {
				if (raw_spin_trylock_irqsave(&cp->lock, flags)) {
					update_cache_slow(cp);
					raw_spin_unlock_irqrestore(&cp->lock,
								   flags);
				}
				/*
				 * Someone else is probably updating the cache.
				 * Just let it do its job and see if the cache
				 * is updated in the next round.
				 */
				continue;
			}
			break;
 		}
 	}
}

/*
 * cpudl_init - initialize the cpudl structure
 * @cp: the cpudl max-heap context
 */
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b))
{
	int i;

	raw_spin_lock_init(&cp->lock);
	atomic_set(&cp->cached_cpu, NO_CACHED_CPU);
	for(i = 0; i < NR_CPUS; i++)
		atomic64_set(&cp->current_dl[i], NO_CACHED_DL);
	
 	cp->cmp_dl = cmp_dl;

	return 0;
}

/*
 * cpudl_cleanup - clean up the cpudl structure
 * @cp: the cpudl max-heap context
 */
void cpudl_cleanup(struct cpudl *cp)
{
	/*
	 * no dynamic memory allocation means nothing to do
	 */
}
