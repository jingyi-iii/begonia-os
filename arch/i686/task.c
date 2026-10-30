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
        return E_INVAL;

    memset(ctx, 0, sizeof(*ctx));

    ctx->ring = priv == TASK_PRIV_KERNEL ? 0 : 3;

    // exec/read code segment, 0 ~ 0xfffff
    ctx->ldts[0] = 0x0000ffff;
    ctx->ldts[1] = 0x00cf9a00 | (ctx->ring << 13);
    // r/w data segment, 0 ~ 0xfffff
    ctx->ldts[2] = 0x0000ffff;
    ctx->ldts[3] = 0x00cf9200 | (ctx->ring << 13);

    ctx->kstack_top = (u32)ctx->kstack + sizeof(ctx->kstack);
    ctx->ustack_top = (u32)ctx->ustack + sizeof(ctx->ustack);

    ctx->code_sel = 0x0 | 0x4 | ctx->ring;
    ctx->data_sel = 0x8 | 0x4 | ctx->ring;
    ctx->entry = (u32)entry;

    u32* sp = (u32*)ctx->kstack_top;
    *--sp = (u32)entry;
    *--sp = 0;  // ebp
    *--sp = 0;  // ebx
    *--sp = 0;  // esi
    *--sp = 0;  // edi

    ctx->esp = (u32)sp;

    return 0;
}

int task_release(task_ctx* ctx)
{
    if (!ctx)
        return E_INVAL;

    memset(ctx, 0, sizeof(task_ctx));
    return 0;
}

void task_switch(task_ctx* from, task_ctx* to)
{
    if (!to)
        return;

    g_tss.esp0 = to->kstack_top;
    ldt_load(to);
    g_task_ctx = to;

    task_switch_asm(from, to);
}

void task_switch_to_ring3(task_ctx* ctx)
{
    if (!ctx)
        return;

    g_tss.esp0 = ctx->kstack_top;
    ldt_load(ctx);
    g_task_ctx = ctx;

    __asm__ __volatile__(
        "movl   %0,   %%eax \n"
        "movw   %%ax, %%ds  \n"
        "movw   %%ax, %%es  \n"
        "movw   %%ax, %%fs  \n"
        "movw   %%ax, %%gs  \n"
        "pushl  %0          \n"   /* SS = 0x0f */
        "pushl  %1          \n"   /* ESP = ustack_top */
        "pushl  $0x00003202 \n"   /* EFLAGS: IF=1, IOPL=3 */
        "pushl  %2          \n"   /* CS = 0x07 */
        "pushl  %3          \n"   /* EIP = entry */
        "iret               \n"
        :: "r"((u32)ctx->data_sel), "r"(ctx->ustack_top),
           "r"((u32)ctx->code_sel), "r"(ctx->entry)
        : "eax", "memory");
    __builtin_unreachable();
}
