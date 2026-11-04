#ifndef SYSCALL_H
#define SYSCALL_H

#include "errno.h"
#include "list.h"
#include "spinlock.h"
#include "task.h"
#include "typedefs.h"
#include "interrupt.h"

typedef int (*sc_handler_fn)(void* arg);

struct syscall {
    i32                 id;
    u32                 param_max_size;
    sc_handler_fn       handler;
    list_node           this_node;
};

int syscall_register(int id, sc_handler_fn handler, u32 param_max_size);
int syscall_unregister(int id);
int syscall_dispatch(u32 id, void* arg, u32 size);

#endif
