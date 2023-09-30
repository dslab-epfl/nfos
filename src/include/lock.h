#ifndef __LOCK_H_
#define __LOCK_H_

// to get NULL
#include <stddef.h>

/* I am assuming, we are using X86 machine */

# define L1D_CACHELINE_SIZE   64
# define ____cacheline_aligned  __attribute__ ((aligned (L1D_CACHELINE_SIZE)))
#define ____cacheline_aligned2 __attribute__ ((aligned (2 * L1D_CACHELINE_SIZE)))

#ifndef smp_wmb
#define smp_wmb() { __asm__ __volatile__("sfence":::"memory"); }
#endif

#ifndef smp_rmb
// Use lfence to be on the safe side
#define smp_rmb() { __asm__ __volatile__("lfence":::"memory"); }
#endif

#ifndef smp_swap
#define smp_swap(__ptr, __val)			\
																__sync_lock_test_and_set(__ptr, __val)
#endif

#ifndef smp_cas
#define smp_cas(__ptr, __old_val, __new_val)	\
																__sync_bool_compare_and_swap(__ptr, __old_val, __new_val)
#endif

#ifndef cpu_pause
#define cpu_pause() { __asm__ __volatile__("pause\n":::"memory"); }
#endif

struct mcsqnode_t;

struct mcslock_t {
  volatile struct mcsqnode_t *qnode;
} ____cacheline_aligned2;

struct mcsqnode_t {
  volatile int locked;
		struct mcsqnode_t *next;
} ____cacheline_aligned;

static inline
void mcslock_init(struct mcslock_t *lock) {
  lock->qnode = NULL;
		smp_wmb();
}

static inline
void mcslock_lock(struct mcslock_t *lock, struct mcsqnode_t *qnode) {
  struct mcsqnode_t *prev;

		qnode->locked = 0;
		qnode->next = NULL;

		prev = (struct mcsqnode_t *)smp_swap(&lock->qnode, qnode);
		if (prev) {
    prev->next = qnode;
				smp_wmb();
				while (!qnode->locked) {
								cpu_pause();
				}
		}
}

static inline
void mcslock_unlock(struct mcslock_t *lock, struct mcsqnode_t *qnode) {
  if (!qnode->next) {
    if (smp_cas(&lock->qnode, qnode, NULL))
								return;

				while (!qnode->next)
								smp_rmb();
		}
		qnode->next->locked = 1;
		smp_wmb();
}


#endif /* __LOCK_H_ */
