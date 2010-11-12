#ifndef _LINUX_CPUDL_H
#define _LINUX_CPUDL_H

#include <linux/sched.h>

#define CPUDL_INVALID	-1
#define IDX_INVALID     -1
#define MAX_CPU         -1

struct array_item {
        u64 dl;
        int cpu;
};
typedef struct array_item item;

struct cpudl {
	raw_spinlock_t lock;
        int size;
	int cpu_to_idx[NR_CPUS];
        item elements[NR_CPUS];
	cpumask_var_t free_cpus;
};


#ifdef CONFIG_SMP
int cpudl_find(struct cpudl *cp, struct cpumask *dlo_mask,
		struct task_struct *p, struct cpumask *later_mask);
void cpudl_set(struct cpudl *cp, int cpu, u64 dl);
int cpudl_init(struct cpudl *cp);
void cpudl_cleanup(struct cpudl *cp);
#else
#define cpudl_set(cp, cpu, dl) do { } while (0)
#define cpudl_init() do { } while (0)
#endif /* CONFIG_SMP */

#endif /* _LINUX_CPUDL_H */

