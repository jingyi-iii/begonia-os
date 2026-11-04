#include "syscall.h"
#include "heap.h"
#include "string.h"
#include "spinlock.h"

static LIST_HEAD(syscalls);
static spinlock splock = SPINLOCK_INIT;

int syscall_register(int id, sc_handler_fn handler, u32 param_max_size)
{
    if (!handler || id < 0)
        return E_INVAL;

    struct syscall* sc = malloc(sizeof(struct syscall));
    if (!sc)
        return E_NOMEM;

    memset(sc, 0, sizeof(struct syscall));
    sc->id = id;
    sc->handler = handler;
    sc->param_max_size = param_max_size;
    list_init(&sc->this_node);

    u32 eflags = spinlock_lock(&splock);
    list_for_each(node, &syscalls) {
        struct syscall* ex = list_entry(node, struct syscall, this_node);
        if (ex->id == id) {
            spinlock_unlock(&splock, eflags);
            free(sc);
            return E_EXISTS;
        }
    }
    list_add(&sc->this_node, &syscalls);
    spinlock_unlock(&splock, eflags);

    return 0;
}

int syscall_unregister(int id)
{
    if (id < 0)
        return E_INVAL;

    u32 eflags = spinlock_lock(&splock);
    list_for_each_safe(node, next, &syscalls) {
        struct syscall* sc = list_entry(node, struct syscall, this_node);
        if (sc->id != id)
            continue;
        
        list_del(&sc->this_node);
        spinlock_unlock(&splock, eflags);
        free(sc);
        return 0;
    }
    spinlock_unlock(&splock, eflags);

    return E_NOTFOUND;
}

int syscall_dispatch(u32 id, void* arg, u32 size)
{
    int ret = E_NOTFOUND;
    sc_handler_fn fn = 0;
    u32 param_max_size = 0;

    u32 eflags = spinlock_lock(&splock);
    list_for_each(node, &syscalls) {
        struct syscall* sc = list_entry(node, struct syscall, this_node);
        if ((u32)sc->id != id)
            continue;

        ret = 0;
        fn = sc->handler;
        param_max_size = sc->param_max_size;
        break;
    }
    spinlock_unlock(&splock, eflags);

    if (ret)
        return ret;
    if (size > param_max_size)
        return E_2BIG;

    return fn(arg);
}