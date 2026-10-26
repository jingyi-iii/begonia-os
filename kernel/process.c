#include "process.h"
#include "heap.h"
#include "log.h"
#include "string.h"
#include "cpu.h"

static DECLARE_HEAD_NODE(threads);
static DECLARE_HEAD_NODE(processes);
static spinlock schedule_lock = SPINLOCK_INIT;
static tcb* g_current_thread = 0;

static inline tcb* thread_get_by_tid(i32 tid)
{
    u32 eflags = spinlock_lock(&schedule_lock);

    list_for_each(node, &threads) {
        tcb* thread = list_entry(node, tcb, node);
        if (thread && thread->tid == tid) {
            spinlock_unlock(&schedule_lock, eflags);
            return thread;
        }
    }
    spinlock_unlock(&schedule_lock, eflags);

    return 0;
}

static inline void thread_set_current(tcb* thread)
{
    if (thread)
        g_current_thread = thread;
}

static inline tcb* thread_get_current(void)
{
    return g_current_thread;
}

static tcb* thread_get_next_runnable(tcb* current)
{
    u32 eflags = spinlock_lock(&schedule_lock);
    list_for_each(node, &threads) {
        tcb* thread = list_entry(node, tcb, node);
        if (thread && thread->state == TS_READY && thread != current) {
            spinlock_unlock(&schedule_lock, eflags);
            return thread;
        }
    }
    spinlock_unlock(&schedule_lock, eflags);
    return 0;
}

static inline pcb* proc_get_by_pid(i32 pid)
{
    u32 eflags = spinlock_lock(&schedule_lock);

    list_for_each(node, &processes) {
        pcb* proc = list_entry(node, pcb, node);
        if (proc && proc->pid == pid) {
            spinlock_unlock(&schedule_lock, eflags);
            return proc;
        }
    }
    spinlock_unlock(&schedule_lock, eflags);

    return 0;
}

static inline void thread_release_zombies(void)
{
    u32 eflags = spinlock_lock(&schedule_lock);

    list_for_each_safe(node, next, &threads) {
        tcb* thread = list_entry(node, tcb, node);
        if (thread->state == TS_ZOMBIE) {
            list_del(&thread->node);
            list_del(&thread->this_child);
            memset(thread, 0, sizeof(tcb));
            free(thread);
        }
    }

    spinlock_unlock(&schedule_lock, eflags);
}

static inline void proc_release_zombies(void)
{
    u32 eflags = spinlock_lock(&schedule_lock);

    list_for_each_safe(node, next, &processes) {
        pcb* proc = list_entry(node, pcb, node);
        if (proc->state != PS_ZOMBIE)
            continue;

        if (list_empty(&proc->children)) {
            list_del(&proc->node);
            free(proc);
        }
    }

    spinlock_unlock(&schedule_lock, eflags); 
}

static int thread_switch(thread_state from_state, tcb* to)
{
    u32 eflags = spinlock_lock(&schedule_lock);

    if (!to || to->state != TS_READY) {
        spinlock_unlock(&schedule_lock, eflags);
        return 0;
    }

    tcb* curr = thread_get_current();
    if (curr)
        curr->state = from_state;
    to->state = TS_RUNNING;
    g_current_thread = to;

    spinlock_unlock(&schedule_lock, eflags);

    task_switch(g_task_ctx, &to->ctx);
    thread_release_zombies();
    proc_release_zombies();

    return 1;
}

void thread_start(void)
{
    if (thread_get_current())
        return;

    if (!thread_switch(TS_NULL, thread_get_next_runnable(0)))
        cpu_halt();
}

static void thread_entry(void)
{
    tcb* self = thread_get_current();

    if (self->priv == THREAD_PRIV_USER)
        task_switch_to_ring3(&self->ctx);

    ((task_entry)self->ctx.entry)();
    thread_exit(self->tid);
}

i32 thread_create(task_priv priv, task_entry entry, void* param, pcb* parent)
{
    static i32 tid = 0;
    int ret = 0;
    u32 eflags = 0;
    tcb* thread = 0;

    thread = (tcb*)malloc(sizeof(tcb));
    if (!thread) {
        LOG("thread_create: failed to alloc memory for tcb\n");
        return E_NOMEM;
    }

    memset(thread, 0, sizeof(tcb));
    ret = task_init(&thread->ctx, thread_entry, priv);
    if (ret) {
        LOG("thread_create: failed to init thread context\n");
        free(thread);
        return ret;
    }

    thread->ctx.entry = (u32)entry;
    thread->tid = tid++;
    thread->priv = (thread_priv)priv;
    thread->parent = parent;
    thread->state = TS_READY;
    list_init(&thread->node);
    list_init(&thread->this_child);

    eflags = spinlock_lock(&schedule_lock);
    if (parent)
        list_add(&thread->this_child, &parent->children);
    list_add(&thread->node, &threads);
    spinlock_unlock(&schedule_lock, eflags);
    return thread->tid;
}

void thread_exit(i32 tid)
{
    tcb* thread = thread_get_by_tid(tid);
    if (!thread)
        return;

    if (thread_get_current() != thread) {
        u32 eflags = spinlock_lock(&schedule_lock);
        list_del(&thread->node);
        list_del(&thread->this_child);
        spinlock_unlock(&schedule_lock, eflags);
        memset(thread, 0, sizeof(tcb));
        free(thread);
        return;
    }

    tcb* next = thread_get_next_runnable(thread);
    if (!next) {
        LOG("thread_exit: last runnable thread, halting\n");
        cpu_halt();
    }

    (void)thread_switch(TS_ZOMBIE, next);
    cpu_halt();
}

void thread_yield(void)
{
    tcb* thread = thread_get_current();
    if (!thread || thread->state != TS_RUNNING)
        return;

    (void)thread_switch(TS_READY, thread_get_next_runnable(thread));
}

void thread_block(i32 tid)
{
    tcb* thread = 0;
    u32 eflags = 0;
    
    thread = thread_get_by_tid(tid);
    if (!thread || (thread->state == TS_PENDING || thread->state == TS_NULL)) {
        LOG("thread_block: no such thread or the thread is already blocked\n");
        return;
    }

    if (thread_get_current() != thread) {
        eflags = spinlock_lock(&schedule_lock);
        thread->state = TS_PENDING;
        spinlock_unlock(&schedule_lock, eflags);
        return;
    }

    (void)thread_switch(TS_PENDING, thread_get_next_runnable(thread));
}

void thread_unblock(i32 tid)
{
    tcb* thread = 0;
    u32 eflags = 0;
    
    thread = thread_get_by_tid(tid);
    if (!thread || thread->state != TS_PENDING)
        return;
    
    eflags = spinlock_lock(&schedule_lock);
    thread->state = TS_READY;
    spinlock_unlock(&schedule_lock, eflags);
}

i32 proc_create(proc_priv priv, task_entry entry, void* param)
{
    static i32 pid = 0;
    int ret = 0;
    pcb* proc = 0;
    u32 eflags = 0;

    proc = (pcb*)malloc(sizeof(pcb));
    if (!proc) {
        LOG("proc_create: failed to alloc memory for pcb\n");
        return E_NOMEM;
    }

    memset(proc, 0, sizeof(pcb));
    proc->pid = pid++;
    proc->priv = priv;
    proc->state = PS_READY;
    list_init(&proc->node);
    list_init(&proc->children);

    ret = thread_create((task_priv)priv, entry, param, proc);
    if (ret < 0) {
        LOG("proc_create: failed to create main thread for process\n");
        free(proc);
        return ret;
    }

    eflags = spinlock_lock(&schedule_lock);
    list_add(&proc->node, &processes);
    spinlock_unlock(&schedule_lock, eflags);

    return proc->pid;
}

void proc_exit(i32 pid)
{
    pcb* proc = 0;
    tcb* curr_thread = 0;
    u32 eflags = 0;
    u8 self_in_del_list = 0;

    proc = proc_get_by_pid(pid);
    if (!proc)
        return;

    eflags = spinlock_lock(&schedule_lock);
    curr_thread = thread_get_current();
    list_for_each_safe(pos, n, &proc->children) {
        tcb* thread = list_entry(pos, tcb, this_child);
        if (thread == curr_thread) {
            self_in_del_list = 1;
            thread->state = TS_ZOMBIE;
            continue;
        }

        // do not call thread_exit() here to prevent schedule
        list_del(&thread->node);
        list_del(&thread->this_child);
        memset(thread, 0, sizeof(tcb));
        free(thread);
    }

    // no children
    if (list_empty(&proc->children)) {
        list_del(&proc->node);
        free(proc);
        spinlock_unlock(&schedule_lock, eflags);
        return;
    }

    // child in running
    proc->state = PS_ZOMBIE;
    spinlock_unlock(&schedule_lock, eflags);
    if (self_in_del_list)
        thread_exit(curr_thread->tid);
    cpu_halt();
}

void proc_block(i32 pid)
{
    u32 eflags = 0;
    pcb* proc = 0;
    u8 need_schedule = 0;

    proc = proc_get_by_pid(pid);
    if (!proc) {
        LOG("proc_block: no such process\n");
        return;
    }

    eflags = spinlock_lock(&schedule_lock);
    if (proc->state != PS_READY) {
        LOG("proc_block: the process is already blocked\n");
        spinlock_unlock(&schedule_lock, eflags);
        return;
    }

    proc->state = PS_PENDING;
    list_for_each(pos, &proc->children) {
        tcb* thread = list_entry(pos, tcb, this_child);
        if (thread == thread_get_current())
            need_schedule = 1;
    }
    spinlock_unlock(&schedule_lock, eflags);

    if (need_schedule) {
        tcb* thread = thread_get_current();
        thread_switch(TS_PENDING, thread_get_next_runnable(thread));
    }
}

void proc_unblock(i32 pid)
{
    u32 eflags = 0;
    pcb* proc = 0;

    proc = proc_get_by_pid(pid);
    if (!proc) {
        LOG("proc_unblock: no such process\n");
        return;
    }

    eflags = spinlock_lock(&schedule_lock);
    if (proc->state != PS_PENDING) {
        LOG("proc_unblock: the process is already unblocked\n");
        spinlock_unlock(&schedule_lock, eflags);
        return;
    }

    proc->state = PS_READY;
    list_for_each(pos, &proc->children) {
        tcb* thread = list_entry(pos, tcb, this_child);
        if (thread->state == TS_PENDING)
            thread->state = TS_READY;
    }
    spinlock_unlock(&schedule_lock, eflags);
}
