#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "list.h"
#include "process.h"
#include "spinlock.h"

typedef struct semaphore {
    int         value;
    int         destroyed;
    wait_queue  wq;
    spinlock    sp_lock;
} semaphore;

semaphore* semaphore_create (int init_value);
int        semaphore_destroy(semaphore* sem);
int        semaphore_wait   (semaphore* sem);
int        semaphore_signal (semaphore* sem);

#endif
