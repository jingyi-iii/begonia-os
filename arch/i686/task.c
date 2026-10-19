#include "task.h"
#include "gdt.h"
#include "string.h"
#include "errno.h"

static volatile tss g_tss = {0};
task_ctx* g_task_ctx = 0;

void tss_init(void)
{
    u16 flags = 0;

    g_tss.ss0 = SEL_SYS_DATA;
    g_tss.esp0 = 0; // need to set value later
    g_tss.iobase = sizeof(tss);

    flags = 0;
    flags |= 0x89;  // 32bit TSS type(0x9), present(0x80)

    gdt_set_entry(GDT_TSS, (gdt_entry)gen_gdt_entry((u32)&g_tss, sizeof(tss), flags));
    __asm__ __volatile__("ltr %0" : : "r"((u16)SEL_TSS) : "memory");
}

void ldt_load(task_ctx* context)
{
    if (!context)
        return;

    gdt_set_entry(GDT_LDT, (gdt_entry)gen_gdt_entry((u32)context->ldts, 2 * 8, 0x0082));
    __asm__ __volatile__("lldt %0" : : "r"((u16)SEL_LDT) : "memory");
}

int task_init(task_ctx* ctx, task_entry entry, task_priv priv)
{
    if (!ctx || !entry)
        return -EINVAL;

    memset(ctx, 0, sizeof(*ctx));

    ctx->ring = priv == TASK_PRIV_KERNEL ? 0 : 3;

    // exec/read code segment, 0 ~ 0xfffff
    ctx->ldts[0] = 0x0000ffff;
    ctx->ldts[1] = 0x00cf9a00 | (ctx->ring << 13);
    // r/w data segment, 0 ~ 0xfffff
    ctx->ldts[2] = 0x0000ffff;
    ctx->ldts[3] = 0x00cf9200 | (ctx->ring << 13);

    ctx->regs = (regs*)((u8*)ctx->stack + sizeof(ctx->stack) - sizeof(regs));
    regs* frame = ctx->regs;

    frame->cs = 0x0 | 0x4 | ctx->ring;
    frame->gs = 0x8 | 0x4 | ctx->ring;
    frame->fs = 0x8 | 0x4 | ctx->ring;
    frame->es = 0x8 | 0x4 | ctx->ring;
    frame->ds = 0x8 | 0x4 | ctx->ring;
    frame->ss = 0x8 | 0x4 | ctx->ring;
    frame->eip = (u32)entry;
    frame->esp = (u32)ctx->stack + sizeof(ctx->stack) - sizeof(regs);

    /* let ring-3 tasks own I/O permissions during bring-up */
    frame->eflags = 0x3202;

    return 0;
}

int task_release(task_ctx* ctx)
{
    if (!ctx)
        return -EINVAL;

    memset(ctx, 0, sizeof(task_ctx));
    return 0;
}

int task_restore(task_ctx* ctx)
{
    if (!ctx)
        return -EINVAL;

    if (ctx->regs && ctx->ring)
        g_tss.esp0 = (u32)ctx->regs + sizeof(regs);

    g_task_ctx = ctx;
    ldt_load(g_task_ctx);

    __asm__ __volatile__(
        "movl   %0,     %%esp\n"
        "popl   %%gs         \n"
        "popl   %%fs         \n"
        "popl   %%es         \n"
        "popl   %%ds         \n"
        "popa                \n"
        "iret                \n"
        "hlt                 \n"
        : : "r"(ctx->regs) : "memory");

    return 0;
}