#include "process.h"
#include "heap.h"
#include "log.h"
#include "string.h"
#include "cpu.h"
#include "irq.h"
#include "syscall.h"
#include "elf.h"
#include "range.h"
#include "multiboot.h"

typedef enum {
    THREAD_CTRL_CREATE = 0,
    THREAD_CTRL_DELETE,
    THREAD_CTRL_YIELD,
    THREAD_CTRL_BLOCK,
    THREAD_CTRL_UNBLOCK,
    THREAD_CTRL_PREPARE_WAIT,
    THREAD_CTRL_WAIT,
    THREAD_CTRL_HAS_WAITERS,
    THREAD_CTRL_WAKE_ONE,
    THREAD_CTRL_WAKE_BY_TID,
    THREAD_CTRL_WAKE_ALL,
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
    pcb*            thread_parent;
} proc_thread_ctrl;

static DECLARE_HEAD_NODE(threads);
static DECLARE_HEAD_NODE(processes);
static spinlock schedule_lock = SPINLOCK_INIT;
static tcb* g_current_thread = 0;
static irq* g_proc_timer_irq = 0;
static vmm_space* g_vmm = 0;

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
            if (thread->waiting_on) {
                list_del(&thread->wait_node);
                thread->waiting_on = 0;
            }
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
        return E_INVAL;
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

    return 0;
}

void thread_start(void)
{
    if (thread_get_current())
        return;

    if (thread_switch(TS_NULL, thread_get_next_runnable(0)))
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
        config.thread_parent = parent;

        int ret = syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        if (ret < 0)
            return ret;

        return config.tid;
    }

    static i32 tid = 0;
    int ret = 0;
    u32 eflags = 0;
    tcb* thread = 0;

    if (!parent) {
        log_err("thread_create: no parent process, return\n");
        return E_PROC_NOTFOUND;
    }

    thread = (tcb*)malloc(sizeof(tcb));
    if (!thread) {
        log_err("thread_create: failed to alloc memory for tcb\n");
        return E_NOMEM;
    }

    memset(thread, 0, sizeof(tcb));
    ret = task_init(&thread->ctx, thread_entry, priv);
    if (ret) {
        log_err("thread_create: failed to init thread context\n");
        free(thread);
        return ret;
    }

    if (priv == TASK_PRIV_USER && g_vmm) {
        u32 stack = (u32)vmm_alloc_and_map_pages(g_vmm, 1, PTE_USER_PAGE);
        if (stack)
            thread->ctx.ustack_top = stack + PAGE_SIZE;
        else
            log_err("thread_create: failed to alloc user stack\n");
    }

    thread->ctx.entry = (u32)entry;
    thread->tid = tid++;
    thread->priv = (thread_priv)priv;
    thread->parent = parent;
    thread->state = TS_READY;
    thread->waiting_on = 0;
    list_init(&thread->node);
    list_init(&thread->this_child);
    list_init(&thread->wait_node);

    eflags = spinlock_lock_irqsave(&schedule_lock);
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
        if (thread->waiting_on) {
            list_del(&thread->wait_node);
            thread->waiting_on = 0;
        }
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        memset(thread, 0, sizeof(tcb));
        free(thread);
        return;
    }

    tcb* next = thread_get_next_runnable(thread);
    if (!next) {
        log_warn("thread_exit: last runnable thread, halting\n");
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
        log_warn("thread_block: no such thread or the thread is already blocked\n");
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
    eflags = spinlock_lock_irqsave(&schedule_lock);
    if (!thread || thread->state != TS_PENDING) {
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }
    
    thread->state = TS_READY;
    spinlock_unlock_irqrestore(&schedule_lock, eflags);
}

i32 thread_prepare_wait(wait_queue* wq)
{
    if (!wq) {
        log_warn("%s: wait queue is null\n", __func__);
        return E_INVAL;
    }

    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_PREPARE_WAIT;
        config.param = (void*)wq;
        return syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
    }

    tcb* thread = thread_get_current();
    if (!thread) {
        log_warn("%s: no current thread\n", __func__);
        return E_NOTFOUND;
    }

    u32 eflags = spinlock_lock_irqsave(&schedule_lock);
    if (thread->waiting_on) {
        log_warn("%s: thread %d is already waiting\n", __func__, thread->tid);
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return E_BUSY;
    }

    thread->waiting_on = wq;
    thread->state = TS_PENDING;
    list_add_tail(&thread->wait_node, &wq->waiters);
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    return 0;
}

i32 thread_wait(void)
{
    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_WAIT;
        return syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
    }

    tcb* thread = thread_get_current();
    if (!thread) {
        log_warn("%s: no current thread\n", __func__);
        return E_NOTFOUND;
    }

    if (thread_switch(TS_PENDING, thread_get_next_runnable(thread))) {
        u32 f = spinlock_lock_irqsave(&schedule_lock);
        thread->state = TS_RUNNING;
        thread->waiting_on = 0;
        list_del(&thread->wait_node);
        spinlock_unlock_irqrestore(&schedule_lock, f);

        log_warn("%s: nothing else can run, not blocking\n", __func__);
        return E_AGAIN;
    }

    return 0;
}

i32 thread_has_waiters(wait_queue* wq)
{
    if (!wq) {
        log_warn("%s: wait queue is null\n", __func__);
        return E_INVAL;
    }

    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_HAS_WAITERS;
        config.param = (void*)wq;
        return syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
    }

    i32 has_waiters = 0;
    u32 eflags = spinlock_lock_irqsave(&schedule_lock);
    has_waiters = !list_empty(&wq->waiters);
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    return has_waiters;
}

void thread_wake_one(wait_queue* wq)
{
    if (!wq) {
        log_warn("%s: wait queue is null\n", __func__);
        return;
    }

    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_WAKE_ONE;
        config.param = (void*)wq;
        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&schedule_lock);
    if (list_empty(&wq->waiters)) {
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    list_for_each_safe(pos, n, &wq->waiters) {
        list_node* node = pos;
        tcb* thread = list_entry(node, tcb, wait_node);

        thread->waiting_on = 0;
        list_del(node);
        if (thread->state == TS_PENDING)
            thread->state = TS_READY;

        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    spinlock_unlock_irqrestore(&schedule_lock, eflags);
}

void thread_wake_by_tid(wait_queue* wq, i32 tid)
{
    if (!wq) {
        log_warn("%s: wait queue is null\n", __func__);
        return;
    }

    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_WAKE_BY_TID;
        config.param = (void*)wq;
        config.tid = tid;
        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&schedule_lock);
    if (list_empty(&wq->waiters)) {
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    list_for_each_safe(pos, n, &wq->waiters) {
        list_node* node = pos;
        tcb* thread = list_entry(node, tcb, wait_node);

        if (thread->tid != tid)
            continue;

        thread->waiting_on = 0;
        list_del(node);
        if (thread->state == TS_PENDING)
            thread->state = TS_READY;
        break;
    }

    spinlock_unlock_irqrestore(&schedule_lock, eflags);
}

void thread_wake_all(wait_queue* wq)
{
    if (!wq) {
        log_warn("%s: wait queue is null\n", __func__);
        return;
    }

    if (!may_run_directly()) {
        proc_thread_ctrl config = {0};
        config.cmd = THREAD_CTRL_WAKE_ALL;
        config.param = (void*)wq;
        syscall(SYSCALL_CTRL_ID_PROC, &config, sizeof(config));
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&schedule_lock);
    if (list_empty(&wq->waiters)) {
        spinlock_unlock_irqrestore(&schedule_lock, eflags);
        return;
    }

    list_for_each_safe(pos, n, &wq->waiters) {
        list_node* node = pos;
        tcb* thread = list_entry(node, tcb, wait_node);

        thread->waiting_on = 0;
        list_del(node);
        if (thread->state == TS_PENDING)
            thread->state = TS_READY;
    }
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
        log_err("proc_create: failed to alloc memory for pcb\n");
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
        log_err("proc_create: failed to create main thread for process\n");
        free(proc);
        return ret;
    }

    eflags = spinlock_lock_irqsave(&schedule_lock);
    list_add(&proc->node, &processes);
    spinlock_unlock_irqrestore(&schedule_lock, eflags);

    return proc->pid;
}


static int elf_load_space(vmm_space* vmm, const void* image, u32 image_size, u32* out_entry)
{
    elf_image img = {0};
    void* dst[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32 va[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32 pa[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32 sz[ELF_MAX_LOAD_SEGMENTS] = {0};
    u32 n = 0;
    int ret;

    if (!vmm || !vmm->pmm || !image || !out_entry)
        return E_INVAL;

    ret = elf_parse(image, image_size, &img);
    if (ret)
        return ret;

    /* the entry point has to land in the user window as well */
    if (!u32_range_within(img.entry, 1, USER_BASE_VA, KERNEL_BASE_VA))
        return E_INVAL;

    for (u32 i = 0; i < img.seg_count; i++) {
        u32 page_base = img.seg[i].vaddr & ~(PAGE_SIZE - 1);
        u32 offset    = img.seg[i].vaddr - page_base;
        u32 size;

        /* user mappings only, and the whole segment has to fit the window */
        if (!u32_range_within(img.seg[i].vaddr, img.seg[i].memsz,
                              USER_BASE_VA, KERNEL_BASE_VA)) {
            ret = E_INVAL;
            goto fail;
        }

        /* a segment may start mid-page and always needs room for its bss tail */
        if (!u32_align_up(offset + img.seg[i].memsz, PAGE_SIZE, &size)) {
            ret = E_INVAL;
            goto fail;
        }

        if (pmm_alloc_blocks(vmm->pmm, size, &pa[n])) {
            ret = E_NOMEM;
            goto fail;
        }

        if (!vmm_map_pages_at(vmm, page_base, pa[n], size / PAGE_SIZE,
                              PTE_USER_PAGE)) {
            pmm_free_blocks(vmm->pmm, pa[n], size);
            ret = E_NOMEM;
            goto fail;
        }

        va[n]  = page_base;
        sz[n]  = size;
        dst[n] = (u8*)pa_to_va(pa[n]) + offset;     /* kernel-side alias */
        n++;
    }

    ret = elf_load_into(image, image_size, &img, dst);
    if (ret)
        goto fail;

    *out_entry = img.entry;
    return 0;

fail:
    while (n > 0) {
        n--;
        vmm_unmap_pages(vmm, va[n]);
        pmm_free_blocks(vmm->pmm, pa[n], sz[n]);
    }

    return ret;
}

i32 proc_load_from_elf(const char* name)
{
    int cnt = mboot_module_count();
    u8* start_va = 0;
    u8* end_va = 0;
    u32 file_size = 0;
    u32 align_size = 0;
    u32 load_pa = 0;
    u32 load_va = 0;
    u32 entry = 0;
    i32 pid = E_NOTFOUND;      /* nothing loaded unless a module matches */
    int i = 0;

    for (i = 0; i < cnt; i++) {
        if (mboot_module_get(i, &start_va, &end_va) != 0)
            continue;
        if (!module_name_matches(mboot_module_name(i), name))
            continue;

        file_size = (u32)end_va - (u32)start_va;
        if (!u32_align_up(file_size, PAGE_SIZE, &align_size))
            continue;

        if (!g_vmm || !g_vmm->pmm ||
            pmm_alloc_blocks(g_vmm->pmm, align_size, &load_pa))
            continue;

        load_va = (u32)pa_to_va(load_pa);
        memcpy((void*)load_va, start_va, file_size);
        memset((void*)load_va + file_size, 0, align_size - file_size);

        int ret = elf_load_space(g_vmm, (void*)load_va, align_size, &entry);
        if (ret) {
            pmm_free_blocks(g_vmm->pmm, load_pa, align_size);
            log_err("%s: elf_load_space('%s') failed, return %d", __func__, name, ret);
            return ret;
        }

        /* elf_load_space() copied the segments into their own pages, so the
         * staging copy is spent now. */
        pmm_free_blocks(g_vmm->pmm, load_pa, align_size);

        pid = proc_create(PROC_PRIV_USER, (task_entry)(uptr)entry, 0);
        if (pid < 0)
            return pid;

        break;
    }

    return pid;
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
        if (thread->waiting_on) {
            list_del(&thread->wait_node);
            thread->waiting_on = 0;
        }
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
        log_warn("proc_block: no such process\n");
        return;
    }

    eflags = spinlock_lock_irqsave(&schedule_lock);
    if (proc->state != PS_READY) {
        log_warn("proc_block: the process is already blocked\n");
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
        log_warn("proc_unblock: no such process\n");
        return;
    }

    eflags = spinlock_lock_irqsave(&schedule_lock);
    if (proc->state != PS_PENDING) {
        log_warn("proc_unblock: the process is already unblocked\n");
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
            int tid = thread_create(cfg->priv, cfg->entry,
                                    cfg->param, cfg->thread_parent);
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
        case THREAD_CTRL_PREPARE_WAIT:
            return thread_prepare_wait((wait_queue*)cfg->param);
        case THREAD_CTRL_WAIT:
            return thread_wait();
        case THREAD_CTRL_HAS_WAITERS:
            return thread_has_waiters((wait_queue*)cfg->param);
        case THREAD_CTRL_WAKE_ONE:
            thread_wake_one((wait_queue*)cfg->param);
            return 0;
        case THREAD_CTRL_WAKE_BY_TID:
            thread_wake_by_tid((wait_queue*)cfg->param, cfg->tid);
            return 0;
        case THREAD_CTRL_WAKE_ALL:
            thread_wake_all((wait_queue*)cfg->param);
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
            log_err("proc_syscall_exec: unknown command %d\n", cfg->cmd);
            return E_INVAL;
    }
}

void process_env_init(vmm_space* space)
{
    g_vmm = space;

    tss_init();
    irq_request(&g_proc_timer_irq, "proc_timer", 0, 0, proc_timer_handler, 0);
    if (g_proc_timer_irq)
        irq_unmask(g_proc_timer_irq);

    syscall_register(SYSCALL_CTRL_ID_PROC, proc_syscall_exec,
                     sizeof(proc_thread_ctrl));
}
