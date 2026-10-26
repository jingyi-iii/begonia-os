#ifndef PROCESS_H
#define PROCESS_H

#include "errno.h"
#include "list.h"
#include "spinlock.h"
#include "task.h"
#include "typedefs.h"

typedef enum {
    THREAD_PRIV_KERNEL = 0,
    THREAD_PRIV_USER   = 1,
} thread_priv;

typedef enum {
    PROC_PRIV_KERNEL = 0,
    PROC_PRIV_USER   = 1,
} proc_priv;

typedef enum {
    TS_NULL = 0,
    TS_READY,
    TS_RUNNING,
    TS_PENDING,
    TS_ZOMBIE,
} thread_state;

typedef enum {
    PS_NULL = 0,
    PS_READY,
    PS_PENDING,
    PS_ZOMBIE,
} proc_state;

typedef struct {
    i32             pid;
    proc_priv       priv;
    proc_state      state;
    void*           param;

    list_node       node;
    list_node       children;
} pcb;

typedef struct {
    i32             tid;
    task_ctx        ctx;
    thread_priv     priv;
    thread_state    state;
    pcb*            parent;

    list_node       node;
    list_node       this_child;
} tcb;

i32     thread_create (task_priv priv, task_entry entry,
                                     void* param, pcb* parent);
void    thread_exit   (i32 tid);
void    thread_yield  (void);
void    thread_block  (i32 tid);
void    thread_unblock(i32 tid);

void    thread_start  (void);

i32     proc_create   (proc_priv priv, task_entry entry, void* param);
void    proc_exit     (i32 pid);
void    proc_block    (i32 pid);
void    proc_unblock  (i32 pid);

#endif
