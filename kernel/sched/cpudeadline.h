#ifndef _LINUX_CPUDL_H
#define _LINUX_CPUDL_H

#include <linux/sched.h>

#define CPUDL_MAX_LEVEL		8
#define IDX_INVALID		-1

struct skiplist_item {
	u64 dl;
	int level;
	struct skiplist_item *next[CPUDL_MAX_LEVEL];
	struct skiplist_item *prev[CPUDL_MAX_LEVEL];
	int cpu;
};

struct cpudl {
	raw_spinlock_t lock;
	struct skiplist_item *head;
	struct skiplist_item *cpu_to_idx[NR_CPUS];
	unsigned int level;
	cpumask_var_t free_cpus;
	bool (*cmp_dl)(u64 a, u64 b);
};


#ifdef CONFIG_SMP
int cpudl_find(struct cpudl *cp, struct task_struct *p,
	       struct cpumask *later_mask);
void cpudl_set(struct cpudl *cp, int cpu, u64 dl, int is_valid);
int cpudl_init(struct cpudl *cp, bool (*cmp_dl)(u64 a, u64 b));
void cpudl_cleanup(struct cpudl *cp);
#else
#define cpudl_set(cp, cpu, dl) do { } while (0)
#define cpudl_init() do { } while (0)
#endif /* CONFIG_SMP */

#endif /* _LINUX_CPUDL_H */
