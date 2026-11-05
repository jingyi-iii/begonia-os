#include "process.h"
#include "heap.h"
#include "log.h"
#include "string.h"
#include "cpu.h"
#include "irq.h"
#include "syscall.h"

typedef enum {
    THREAD_CTRL_CREATE = 0,
    THREAD_CTRL_DELETE,
    THREAD_CTRL_YIELD,
    THREAD_CTRL_BLOCK,
    THREAD_CTRL_UNBLOCK,
    PROC_CTRL_CREATE,
    PROC_CTRL_EXIT,
    PROC_CTRL_BLOCK,
    PROC_CTRL_UNBLOCK,
} proc_thread_cmd;

typedef struct {
    proc_thread_cmd cmd;
    i32             pid;    // out param for create, in param for delete, block and unblock
    i32             tid;    // out param for create, in param for delete, block and unblock
    task_priv       priv;
    task_entry      entry;
    void*           param;
} proc_thread_ctrl;

static DECLARE_HEAD_NODE(threads);
static DECLARE_HEAD_NODE(processes);
static spinlock schedule_lock = SPINLOCK_INIT;
static tcb* g_current_thread = 0;
static irq* g_proc_timer_irq = 0;

static inline int may_run_directly(void)
{
    return is_user_mode() == 0;
}

static inline tcb* thread_get_by_tid(i32 tid)
{
    u32 eflags = spinlock_lock_irqsave(&schedule_lock);

    list_for_each(node, &threads) {
        tcb* thread = list_entry(node, tcb, node);
        if (thread && thread->tid == tid) {
            spinlock_unlock_irqrestore(&schedule_lock, eflags);
            return thread;
        }
    }
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

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
    list_node* head;
    list_node* start;
    tcb* found = 0;
    u32 eflags;

    eflags = spinlock_lock_irqsave(&schedule_lock);

    head = &threads;
    start = current ? current->node.next : head->next;

    /* Round robin: the first ready thread after the current one wins,
     * so a thread that just yielded queues up behind its peers instead
     * of being picked again.  The scan wraps once to cover the threads
     * ahead of the current one. */
    for (list_node* node = start; node != head; node = node->next) {
        tcb* thread = list_entry(node, tcb, node);
        if (thread->state == TS_READY) {
            found = thread;
            break;
        }
    }

    if (!found && current) {
        for (list_node* node = head->next; node != &current->node;
             node = node->next) {
            tcb* thread = list_entry(node, tcb, node);
            if (thread->state == TS_READY) {
                found = thread;
                break;
            }
        }
    }

    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    return found;
}

static inline pcb* proc_get_by_pid(i32 pid)
{
    u32 eflags = spinlock_lock_irqsave(&schedule_lock);

    list_for_each(node, &processes) {
        pcb* proc = list_entry(node, pcb, node);
        if (proc && proc->pid == pid) {
            spinlock_unlock_irqrestore(&schedule_lock, eflags);
            return proc;
        }
    }
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    return 0;
}

static inline void thread_release_zombies(void)
{
    u32 eflags = spinlock_lock_irqsave(&schedule_lock);

    list_for_each_safe(node, next, &threads) {
        tcb* thread = list_entry(node, tcb, node);
        if (thread->state == TS_ZOMBIE) {
            list_del(&thread->node);
            list_del(&thread->this_child);
            memset(thread, 0, sizeof(tcb));
            free(thread);
        }
    }

    spinlock_unlock_irqrestore(&schedule_lock, eflags);
}

static inline void proc_release_zombies(void)
{
    u32 eflags = spinlock_lock_irqsave(&schedule_lock);

    list_for_each_safe(node, next, &processes) {
        pcb* proc = list_entry(node, pcb, node);
        if (proc->state != PS_ZOMBIE)
            continue;

        if (list_empty(&proc->children)) {
            list_del(&proc->node);
            free(proc);
        }
    }

    spinlock_unlock_irqrestore(&schedule_lock, eflags); 
}

static int thread_switch(thread_state from_state, tcb* to)
{
    u32 eflags = spinlock_lock_irqsave(&schedule_lock);

    if (!to || to->state != TS_READY) {
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return 0;
    }

    tcb* curr = thread_get_current();
    if (curr)
        curr->state = from_state;
    to->state = TS_RUNNING;
    g_current_thread = to;

    spinlock_unlock_irqrestore(&schedule_lock, eflags);

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
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_CREATE;
        config.priv = (task_priv)priv;
        config.entry = entry;
        config.param = param;

        int ret = syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        if (ret < 0)
            return ret;

        return config.tid;
    }

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

    eflags = spinlock_lock_irqsave(&schedule_lock);
    if (parent)
        list_add(&thread->this_child, &parent->children);
    list_add(&thread->node, &threads);
    spinlock_unlock_irqrestore(&schedule_lock, eflags);
    return thread->tid;
}

void thread_exit(i32 tid)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_DELETE;
        config.tid = tid;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    tcb* thread = thread_get_by_tid(tid);
    if (!thread)
        return;

    if (thread_get_current() != thread) {
        u32 eflags = spinlock_lock_irqsave(&schedule_lock);
        list_del(&thread->node);
        list_del(&thread->this_child);
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
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
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_YIELD;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    tcb* thread = thread_get_current();
    if (!thread || thread->state != TS_RUNNING)
        return;

    (void)thread_switch(TS_READY, thread_get_next_runnable(thread));
}

void thread_block(i32 tid)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_BLOCK;
        config.tid = tid;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    tcb* thread = 0;
    u32 eflags = 0;
    
    thread = thread_get_by_tid(tid);
    if (!thread || (thread->state == TS_PENDING || thread->state == TS_NULL)) {
        LOG("thread_block: no such thread or the thread is already blocked\n");
        return;
    }

    if (thread_get_current() != thread) {
        eflags = spinlock_lock_irqsave(&schedule_lock);
        thread->state = TS_PENDING;
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    (void)thread_switch(TS_PENDING, thread_get_next_runnable(thread));
}

void thread_unblock(i32 tid)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_UNBLOCK;
        config.tid = tid;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    tcb* thread = 0;
    u32 eflags = 0;
    
    thread = thread_get_by_tid(tid);
    if (!thread || thread->state != TS_PENDING)
        return;
    
    eflags = spinlock_lock_irqsave(&schedule_lock);
    thread->state = TS_READY;
    spinlock_unlock_irqrestore(&schedule_lock, eflags);
}

i32 proc_create(proc_priv priv, task_entry entry, void* param)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = PROC_CTRL_CREATE;
        config.priv = (task_priv)priv;
        config.entry = entry;
        config.param = param;

        return syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
    }

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

    eflags = spinlock_lock_irqsave(&schedule_lock);
    list_add(&proc->node, &processes);
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    return proc->pid;
}

void proc_exit(i32 pid)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = PROC_CTRL_EXIT;
        config.pid = pid;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    pcb* proc = 0;
    tcb* curr_thread = 0;
    u32 eflags = 0;
    u8 self_in_del_list = 0;

    proc = proc_get_by_pid(pid);
    if (!proc)
        return;

    eflags = spinlock_lock_irqsave(&schedule_lock);
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
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    // child in running
    proc->state = PS_ZOMBIE;
    spinlock_unlock_irqrestore(&schedule_lock, eflags);
    if (self_in_del_list)
        thread_exit(curr_thread->tid);
    cpu_halt();
}

void proc_block(i32 pid)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = PROC_CTRL_BLOCK;
        config.pid = pid;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    u32 eflags = 0;
    pcb* proc = 0;
    u8 need_schedule = 0;

    proc = proc_get_by_pid(pid);
    if (!proc) {
        LOG("proc_block: no such process\n");
        return;
    }

    eflags = spinlock_lock_irqsave(&schedule_lock);
    if (proc->state != PS_READY) {
        LOG("proc_block: the process is already blocked\n");
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    proc->state = PS_PENDING;
    list_for_each(pos, &proc->children) {
        tcb* thread = list_entry(pos, tcb, this_child);
        if (thread == thread_get_current())
            need_schedule = 1;
    }
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    if (need_schedule) {
        tcb* thread = thread_get_current();
        thread_switch(TS_PENDING, thread_get_next_runnable(thread));
    }
}

void proc_unblock(i32 pid)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = PROC_CTRL_UNBLOCK;
        config.pid = pid;

        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    u32 eflags = 0;
    pcb* proc = 0;

    proc = proc_get_by_pid(pid);
    if (!proc) {
        LOG("proc_unblock: no such process\n");
        return;
    }

    eflags = spinlock_lock_irqsave(&schedule_lock);
    if (proc->state != PS_PENDING) {
        LOG("proc_unblock: the process is already unblocked\n");
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    proc->state = PS_READY;
    list_for_each(pos, &proc->children) {
        tcb* thread = list_entry(pos, tcb, this_child);
        if (thread->state == TS_PENDING)
            thread->state = TS_READY;
    }
    spinlock_unlock_irqrestore(&schedule_lock, eflags);
}

static void proc_timer_handler(void* context)
{
    (void)context;

    static u32 timeslice = 0;
    timeslice++;

    if (timeslice < 10)
        return;

    timeslice = 0;
    LOG("proc_timer_handler: timer interrupt, yield current thread\n");
    thread_yield();
}

static int proc_syscall_exec(void* data)
{
    proc_thread_ctrl* cfg = (proc_thread_ctrl*)data;
    if (!cfg)
        return E_INVAL;

    switch (cfg->cmd) {
        case THREAD_CTRL_CREATE: {
            int tid = thread_create(cfg->priv, cfg->entry, cfg->param, 0);
            if (tid < 0)
                return tid;

            cfg->tid = tid;
            return 0;
        }
        case THREAD_CTRL_DELETE:
            thread_exit(cfg->tid);
            return 0;
        case THREAD_CTRL_YIELD:
            thread_yield();
            return 0;
        case THREAD_CTRL_BLOCK:
            thread_block(cfg->tid);
            return 0;
        case THREAD_CTRL_UNBLOCK:
            thread_unblock(cfg->tid);
            return 0;
        case PROC_CTRL_CREATE:
            return proc_create((proc_priv)cfg->priv, cfg->entry, cfg->param);
        case PROC_CTRL_EXIT:
            proc_exit(cfg->pid);
            return 0;
        case PROC_CTRL_BLOCK:
            proc_block(cfg->pid);
            return 0;
        case PROC_CTRL_UNBLOCK:
            proc_unblock(cfg->pid);
            return 0;
        default:
            LOG("proc_syscall_exec: unknown command %d\n", cfg->cmd);
            return E_INVAL;
    }
}

void process_env_init(void)
{
    tss_init();
    irq_request(&g_proc_timer_irq, "proc_timer", 0, 0, proc_timer_handler, 0);
    if (g_proc_timer_irq)
        irq_unmask(g_proc_timer_irq);

    syscall_register(SYSCALL_CTRL_ID_PROC, proc_syscall_exec,
                     sizeof(proc_thread_ctrl));
}
