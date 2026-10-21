#ifndef ARCH_TASK_H
#define ARCH_TASK_H

#include "typedefs.h"

typedef struct regs {
    u32     gs;
    u32     fs;
    u32     es;
    u32     ds;
    u32     edi;
    u32     esi;
    u32     ebp;
    u32     kesp;
    u32     ebx;
    u32     edx;
    u32     ecx;
    u32     eax;
    u32     eip;
    u32     cs;
    u32     eflags;
    u32     esp;
    u32     ss;
} __attribute__((packed)) regs;

typedef struct tss {
    u32     backlink;
    u32     esp0;
    u32     ss0;
    u32     esp1;
    u32     ss1;
    u32     esp2;
    u32     ss2;
    u32     cr3;
    u32     eip;
    u32     flags;
    u32     eax;
    u32     ecx;
    u32     edx;
    u32     ebx;
    u32     esp;
    u32     ebp;
    u32     esi;
    u32     edi;
    u32     es;
    u32     cs;
    u32     ss;
    u32     ds;
    u32     fs;
    u32     gs;
    u32     ldt;
    u16     trap;
    u16     iobase;
} __attribute__((packed)) tss;

typedef enum task_priv {
    TASK_PRIV_KERNEL = 0,
    TASK_PRIV_USER,
} task_priv;

typedef struct task_ctx {
    regs*   regs;
    u32     ldts[4];        // 64 bits for each ldt entry
    u8      stack[4096];    // Fixed size currently. Refactor to use dynamic allocation later.
    u8      ring;
} task_ctx;

typedef void (*task_entry)(void);

void tss_init(void);
void ldt_load(task_ctx* context);

int  task_init(task_ctx* ctx, task_entry entry, task_priv priv);
int  task_release(task_ctx* ctx);
int  task_restore(task_ctx* ctx);

#endif
