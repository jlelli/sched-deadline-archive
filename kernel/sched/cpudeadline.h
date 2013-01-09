#ifndef _LINUX_CPUDL_H
#define _LINUX_CPUDL_H

#include <linux/cpumask.h>
#include <linux/types.h>

#define NO_CACHED_CPU		-1
#define NO_CPU_DL		-2
#define NO_CACHED_DL		0

struct cpudl {
	raw_spinlock_t lock;
	cpumask_var_t free_cpus;
	atomic_t cached_cpu;
	atomic64_t current_dl[NR_CPUS];

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
