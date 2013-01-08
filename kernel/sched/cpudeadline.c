/*
 *  kernel/sched/cpudeadline.c
 *
 *  Global CPU deadline management
 *
 *  Author: Juri Lelli <j.lelli@sssup.it>
 *  Author: Fabio Falzoi <falzoi@tecip.sssup.it>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 */

#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "cpudeadline.h"

#define LEVEL_PROB_VALUE	0.20
#define NOT_IN_LIST		-1
#define CPUDL_RAND_MAX		~0UL
#define CPUDL_HEAD_IDX		-1

static inline u64 cpudl_detach(struct cpudl *list, struct skiplist_item *p)
{
	int i;

	for(i = 0; i <= p->level; i++) {
		p->prev[i]->next[i] = p->next[i];
		if(p->next[i])
			p->next[i]->prev[i] = p->prev[i];
	}

	while(!list->head->next[list->level] && list->level > 0)
		list->level--;

	p->level = NOT_IN_LIST;

	return p->dl;
}

static u64 cpudl_remove_idx(struct cpudl *list, const int cpu)
{
	struct skiplist_item *p;

	p = list->cpu_to_idx[cpu];

	if(p->level == NOT_IN_LIST)
		return 0;

	cpumask_set_cpu(cpu, list->free_cpus);

	return cpudl_detach(list, p);
}

static inline unsigned int cpudl_rand_level(unsigned int max)
{
	unsigned int level = 0;
	struct timespec limit;

	max = max > CPUDL_MAX_LEVEL ? CPUDL_MAX_LEVEL - 1 : max;

	limit = current_kernel_time();
	level = (unsigned int)(limit.tv_nsec);

	return level % max;
}

static int cpudl_insert(struct cpudl *list, const int cpu, u64 dl)
{
	struct skiplist_item *p;
	struct skiplist_item *update[CPUDL_MAX_LEVEL];
	struct skiplist_item *new_node;
	int cmp_res, level, i;
	unsigned int rand_level;

	new_node = list->cpu_to_idx[cpu];

	new_node->dl = dl;

	p = list->head;
	level = list->level;
	while(level >= 0) {
		update[level] = p;

		if(!p->next[level]) {
			level--;
			continue;
		}

		cmp_res = list->cmp_dl(new_node->dl, p->next[level]->dl);

		if(cmp_res > 0)
			p = p->next[level];
		else
			level--;
	}

	rand_level = cpudl_rand_level(list->level + 1);
	new_node->level = rand_level;

	if(rand_level > list->level)
		update[++list->level] = list->head;

	for(i = 0; i <= rand_level; i++) {
		new_node->next[i] = update[i]->next[i];
		update[i]->next[i] = new_node;
		new_node->prev[i] = update[i];
		if(new_node->next[i])
			new_node->next[i]->prev[i] = new_node;
	}

	cpumask_clear_cpu(cpu, list->free_cpus);

	return 0;
}

/*
 * cpudl_find - find the best (later-dl) CPU in the system
 * @cp: the cpudl skiplist context
 * @p: the task
 * @later_mask: a mask to fill in with the selected CPUs (or NULL)
 *
 * Returns: int - best CPU (skiplist maximum if suitable)
 */
int cpudl_find(struct cpudl *cp, struct task_struct *p,
	       struct cpumask *later_mask)
{
	struct skiplist_item *first;
	u64 first_dl;
	int first_cpu, best_cpu = -1;
	const struct sched_dl_entity *dl_se;

	if(p)
		dl_se = &p->dl;

	if(later_mask && cpumask_and(later_mask, cp->free_cpus,
			&p->cpus_allowed) && cpumask_and(later_mask,
			later_mask, cpu_active_mask)) {
		best_cpu = cpumask_any(later_mask);
	} else {
		first = cp->head->next[0];
		if(!first)
			return -1;

		first_cpu = first->cpu;
		first_dl = first->dl;
	
		/*
		 * id cpudl_find is called on behalf of
		 * a pull attempt, we can not do any other
		 * check, so we return immediately
		 * the CPU value from cpudl structure
		 */
		if(!p)
			return first_cpu;
	
		if(cpumask_test_cpu(first_cpu, &p->cpus_allowed) &&
			cp->cmp_dl(dl_se->deadline, first_dl)) {
				best_cpu = first_cpu;
				if(later_mask)
					cpumask_set_cpu(best_cpu, later_mask);
		}
	}

	return best_cpu;
}

/*
 * cpudl_set - update the cpudl skiplist
 * @cp: the cpudl skiplist context
 * @cpu: the target cpu
 * @dl: the new earliest deadline for this cpu
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&cp->lock, flags);
	cpudl_remove_idx(cp, cpu);
	if(is_valid)
		cpudl_insert(cp, cpu, dl);

	raw_spin_unlock_irqrestore(&cp->lock, flags);
}

/*
 * cpudl_init - initialize the cpudl structure
 * @cp: the cpudl skiplist context
 * @cmp_dl: function used to order deadlines inside structure
 */
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b))
{
	int i;

	memset(cp, 0, sizeof(*cp));
	cp->cmp_dl = cmp_dl;
	raw_spin_lock_init(&cp->lock);
	cp->head = (struct skiplist_item *)kmalloc(sizeof(*cp->head), GFP_KERNEL);
	memset(cp->head, 0, sizeof(*cp->head));
	cp->head->cpu = CPUDL_HEAD_IDX;

	memset(cp->cpu_to_idx, 0, sizeof(*cp->cpu_to_idx) * NR_CPUS);

	for(i = 0; i < NR_CPUS; i++) {
		cp->cpu_to_idx[i] = (struct skiplist_item *)
			kmalloc(sizeof(*cp->cpu_to_idx[i]), GFP_KERNEL);
		memset(cp->cpu_to_idx[i], 0, sizeof(*cp->cpu_to_idx[i]));
		cp->cpu_to_idx[i]->level = NOT_IN_LIST;
		cp->cpu_to_idx[i]->cpu = i;
	}

	if(!alloc_cpumask_var(&cp->free_cpus, GFP_KERNEL))
		return -ENOMEM;
	cpumask_setall(cp->free_cpus);

	return 0;
}

/*
 * cpudl_cleanup - clean up the cpudl structure
 * @cp: the cpudl skiplist context
 */
void cpudl_cleanup(struct cpudl *cp)
{
	int i;

	for(i = 0; i < NR_CPUS; i++)
		kfree(cp->cpu_to_idx[i]);

	kfree(cp->head);
}
