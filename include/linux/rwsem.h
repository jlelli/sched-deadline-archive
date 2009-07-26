/* rwsem.h: R/W semaphores, public interface
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <linux/linkage.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <asm/system.h>
#include <asm/atomic.h>

struct rw_anon_semaphore;
struct rw_semaphore;

#ifdef CONFIG_RWSEM_GENERIC_SPINLOCK
#include <linux/rwsem-spinlock.h> /* use a generic implementation */
#else
/* All arch specific implementations share the same struct */
struct rw_anon_semaphore {
	long			count;
	raw_spinlock_t		wait_lock;
	struct list_head	wait_list;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

extern struct rw_anon_semaphore *rwsem_down_read_failed(struct rw_anon_semaphore *sem);
extern struct rw_anon_semaphore *rwsem_down_write_failed(struct rw_anon_semaphore *sem);
extern struct rw_anon_semaphore *rwsem_wake(struct rw_anon_semaphore *);
extern struct rw_anon_semaphore *rwsem_downgrade_wake(struct rw_anon_semaphore *sem);

/* Include the arch specific part */
#include <asm/rwsem.h>

/* In all implementations count != 0 means locked */
static inline int anon_rwsem_is_locked(struct rw_anon_semaphore *sem)
{
	return sem->count != 0;
}

struct rw_semaphore {
	long			count;
	raw_spinlock_t		wait_lock;
	struct list_head	wait_list;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

#endif

/* Common initializer macros and functions */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define __RWSEM_DEP_MAP_INIT(lockname) , .dep_map = { .name = #lockname }
#else
# define __RWSEM_DEP_MAP_INIT(lockname)
#endif

#define __RWSEM_ANON_INITIALIZER(name)			\
	{ RWSEM_UNLOCKED_VALUE,				\
	  __RAW_SPIN_LOCK_UNLOCKED(name.wait_lock),	\
	  LIST_HEAD_INIT((name).wait_list)		\
	  __RWSEM_DEP_MAP_INIT(name) }

#define DECLARE_ANON_RWSEM(name) \
	struct rw_anon_semaphore name = __RWSEM_INITIALIZER(name)

extern void __init_anon_rwsem(struct rw_anon_semaphore *sem, const char *name,
			      struct lock_class_key *key);

#define init_anon_rwsem(sem)					\
do {								\
	static struct lock_class_key __key;			\
								\
	__init_anon_rwsem((sem), #sem, &__key);			\
} while (0)

/*
 * lock for reading
 */
extern void anon_down_read(struct rw_anon_semaphore *sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int anon_down_read_trylock(struct rw_anon_semaphore *sem);

/*
 * lock for writing
 */
extern void anon_down_write(struct rw_anon_semaphore *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int anon_down_write_trylock(struct rw_anon_semaphore *sem);

/*
 * release a read lock
 */
extern void anon_up_read(struct rw_anon_semaphore *sem);

/*
 * release a write lock
 */
extern void anon_up_write(struct rw_anon_semaphore *sem);

/*
 * downgrade write lock to read lock
 */
extern void anon_downgrade_write(struct rw_anon_semaphore *sem);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * nested locking. NOTE: rwsems are not allowed to recurse
 * (which occurs if the same task tries to acquire the same
 * lock instance multiple times), but multiple locks of the
 * same lock class might be taken, if the order of the locks
 * is always the same. This ordering rule can be expressed
 * to lockdep via the _nested() APIs, but enumerating the
 * subclasses that are used. (If the nesting relationship is
 * static then another method for expressing nested locking is
 * the explicit definition of lock class keys and the use of
 * lockdep_set_class() at lock initialization time.
 * See Documentation/lockdep-design.txt for more details.)
 */
extern void anon_down_read_nested(struct rw_anon_semaphore *sem, int subclass);
extern void anon_down_write_nested(struct rw_anon_semaphore *sem, int subclass);
/*
 * Take/release a lock when not the owner will release it.
 *
 * [ This API should be avoided as much as possible - the
 *   proper abstraction for this case is completions. ]
 */
extern void anon_down_read_non_owner(struct rw_anon_semaphore *sem);
extern void anon_up_read_non_owner(struct rw_anon_semaphore *sem);
#else
# define anon_down_read_nested(sem, subclass)	anon_down_read(sem)
# define anon_down_write_nested(sem, subclass)	anon_down_write(sem)
# define anon_down_read_non_owner(sem)		anon_down_read(sem)
# define anon_up_read_non_owner(sem)		anon_up_read(sem)
#endif

/*
 * Non preempt-rt implementations
 */
#define __RWSEM_INITIALIZER(name)			\
	{ RWSEM_UNLOCKED_VALUE,				\
	  __RAW_SPIN_LOCK_UNLOCKED(name.wait_lock),	\
	  LIST_HEAD_INIT((name).wait_list)		\
	  __RWSEM_DEP_MAP_INIT(name) }

#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

static inline void __init_rwsem(struct rw_semaphore *sem, const char *name,
				struct lock_class_key *key)
{
	__init_anon_rwsem((struct rw_anon_semaphore *)sem, name, key);
}

#define init_rwsem(sem)						\
do {								\
	static struct lock_class_key __key;			\
								\
	__init_rwsem((sem), #sem, &__key);			\
} while (0)

static inline void down_read(struct rw_semaphore *sem)
{
	anon_down_read((struct rw_anon_semaphore *)sem);
}

static inline int down_read_trylock(struct rw_semaphore *sem)
{
	return anon_down_read_trylock((struct rw_anon_semaphore *)sem);
}

static inline void down_write(struct rw_semaphore *sem)
{
	anon_down_write((struct rw_anon_semaphore *)sem);
}

static inline int down_write_trylock(struct rw_semaphore *sem)
{
	return anon_down_write_trylock((struct rw_anon_semaphore *)sem);
}

static inline void up_read(struct rw_semaphore *sem)
{
	anon_up_read((struct rw_anon_semaphore *)sem);
}

static inline void up_write(struct rw_semaphore *sem)
{
	anon_up_write((struct rw_anon_semaphore *)sem);
}

static inline void downgrade_write(struct rw_semaphore *sem)
{
	anon_downgrade_write((struct rw_anon_semaphore *)sem);
}

static inline void down_read_nested(struct rw_semaphore *sem, int subclass)
{
	return anon_down_read_nested((struct rw_anon_semaphore *)sem, subclass);
}

static inline void down_write_nested(struct rw_semaphore *sem, int subclass)
{
	anon_down_write_nested((struct rw_anon_semaphore *)sem, subclass);
}

static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return anon_rwsem_is_locked((struct rw_anon_semaphore *)sem);
}

#endif /* _LINUX_RWSEM_H */
