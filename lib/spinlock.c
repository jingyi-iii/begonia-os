#include "spinlock.h"
#include "cpu.h"
#include "errno.h"

u32 spinlock_lock(spinlock* lock)
{
    u32 flags = cpu_irq_save();

    for (;;) {
        /* Look before exchanging: on SMP this keeps the cache line from
         * bouncing between spinning cores. */
        if (!__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) &&
            !__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE))
            return flags;

        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
            cpu_relax();
    }
}

/* Release before restoring the flags: re-enabling interrupts first would let
 * a handler run while the lock is still held, and it would spin forever. */
void spinlock_unlock(spinlock* lock, u32 flags)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    cpu_irq_restore(flags);
}

int spinlock_trylock(spinlock* lock, u32* flags)
{
    u32 expected = 0;
    u32 saved = cpu_irq_save();

    if (__atomic_compare_exchange_n(&lock->locked, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        *flags = saved;
        return 0;
    }

    cpu_irq_restore(saved);
    return -EBUSY;
}
