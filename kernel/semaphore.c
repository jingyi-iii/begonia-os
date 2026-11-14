#include "semaphore.h"
#include "log.h"
#include "heap.h"
#include "string.h"

semaphore* semaphore_create(int init_value)
{
    if (init_value < 0)
        return 0;

    semaphore* sem = (semaphore*)malloc(sizeof(semaphore));
    if (!sem)
        return 0;

    memset(sem, 0, sizeof(semaphore));
    sem->value = init_value;
    wait_queue_init(&sem->wq);
    spinlock_init(&sem->sp_lock);
    return sem;
}

int semaphore_destroy(semaphore* sem)
{
    if (!sem)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&sem->sp_lock);
    int busy = thread_has_waiters(&sem->wq);
    if (!busy)
        sem->destroyed = 1;
    spinlock_unlock_irqrestore(&sem->sp_lock, eflags);

    if (busy)
        return E_BUSY;

    free(sem);
    return 0;
}

int semaphore_wait(semaphore* sem)
{
    int ret = 0;
    if (!sem)
        return E_INVAL;

    for ( ;; ) {
        u32 eflags = spinlock_lock_irqsave(&sem->sp_lock);
        if (sem->destroyed) {
            spinlock_unlock_irqrestore(&sem->sp_lock, eflags);
            return E_DEV_NOTREADY;
        }

        if (sem->value > 0) {
            sem->value--;
            spinlock_unlock_irqrestore(&sem->sp_lock, eflags);
            return 0;
        }
        ret = thread_prepare_wait(&sem->wq);
        spinlock_unlock_irqrestore(&sem->sp_lock, eflags);
        if (ret)
            return ret;

        ret = thread_wait();
        if (ret)
            return ret;
    }

    return 0;
}

int semaphore_signal(semaphore* sem)
{
    if (!sem)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&sem->sp_lock);
    if (sem->destroyed) {
        spinlock_unlock_irqrestore(&sem->sp_lock, eflags);
        return E_DEV_NOTREADY;
    }

    sem->value++;
    spinlock_unlock_irqrestore(&sem->sp_lock, eflags);

    thread_wake_one(&sem->wq);
    return 0;
}
