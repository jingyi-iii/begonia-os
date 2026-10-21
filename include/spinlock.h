#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "typedefs.h"

/* A lock is a value embedded in whatever it protects; there is no pool to
 * allocate from and no capacity to run out of.
 *
 * Taking one disables interrupts first.  On this uniprocessor kernel a spin
 * can only ever block on an interrupt handler that wants the same lock, so a
 * variant that leaves IF alone would turn that handler into a deadlock.
 *
 * spinlock_lock() returns the caller's EFLAGS and spinlock_unlock() takes it
 * back, so nested sections restore the state each one found, and a forgotten
 * restore does not compile. */
typedef struct {
    volatile u32 locked;        /* 0 free, 1 held */
} spinlock;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock* lock)
{
    lock->locked = 0;
}

u32  spinlock_lock(spinlock* lock);
void spinlock_unlock(spinlock* lock, u32 flags);

/* 0 when the lock was taken, -EBUSY when someone else holds it.  On success
 * *flags receives what spinlock_unlock() must be given. */
int  spinlock_trylock(spinlock* lock, u32* flags);

#endif
