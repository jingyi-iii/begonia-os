#include "typedefs.h"
#include "com.h"
#include "pic.h"

#define IDT_GATE_INT32   0xE    /* 32-bit interrupt gate: IF cleared on entry */
#define IDT_GATE_TRAP32  0xF    /* 32-bit trap gate: IF left alone            */

typedef struct {
    u16 limit;
    u32 base;
} __attribute__((packed)) idt_meta;

typedef union {
    u64 raw;
    struct {
        u32 isr_low   : 16;
        u32 sel_code  : 16;
        u32 reserved  : 8;      /* 0 on i686; IST lives here in long mode     */
        u32 gate_type : 4;      /* IDT_GATE_INT32 / IDT_GATE_TRAP32           */
        u32 s         : 1;      /* 0: a gate is a system descriptor, not code */
        u32 dpl       : 2;      /* 0: ring 0 only                             */
        u32 present   : 1;
        u32 isr_high  : 16;
    } __attribute__((packed));
} idt_entry;

/* What isr_common in interrupt.S leaves on the stack.  Keep in step
 * with the layout comment at the top of that file. */
typedef struct {
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha                  */
    u32 vector, err;                              /* slot, then CPU or 0    */
    u32 eip, cs, eflags;                          /* pushed by the CPU      */
} isr_frame;

_Static_assert(sizeof(isr_frame) == 52, "isr_frame must match interrupt.S");

static idt_entry idt[256] = {0};
static idt_meta g_idtmeta = {
    .limit = sizeof(idt_entry) * 256 - 1,
    .base = (u32)idt
};

static void idt_set_gate(u32 vector, u32 handler, u16 sel, u8 dpl)
{
    idt[vector] = (idt_entry){
        .isr_low   = handler & 0xFFFF,
        .isr_high  = handler >> 16,
        .sel_code  = sel,
        .gate_type = IDT_GATE_INT32,
        .dpl       = dpl,
        .present   = 1,
    };
}

void idt_load(void)
{
    __asm__ __volatile__ ("lidt %0" : : "m"(g_idtmeta) : "memory");
}

/* The stub table in interrupt.S: ISR_COUNT slots of ISR_SLOT bytes each,
 * so vector N is entered at isr_tbl + N * ISR_SLOT.  Keep both numbers in
 * step with that file. */
extern u8 isr_tbl[];
#define ISR_SLOT        0x90    /* must match .set ISR_SLOT in interrupt.S */
#define ISR_VECTORS     256     /* one gate per IDT entry                  */
#define SEL_KERNEL_CODE 0x08    /* GDT[1], the flat kernel code segment    */

/* Point every gate at its stub and hand the table to the CPU.  All 256
 * vectors go in with DPL 0, so an exception or a stray IRQ lands in the
 * stub table instead of triple-faulting.  A vector that user mode has to
 * be able to call gets re-installed later with idt_set_gate(.., 3). */
void interrupt_init(void)
{
    u32 vector;

    for (vector = 0; vector < ISR_VECTORS; vector++)
        idt_set_gate(vector, (u32)isr_tbl + vector * ISR_SLOT,
                     SEL_KERNEL_CODE, 0);

    /* The 8259s come out of the firmware at the BIOS default vectors
     * (0x08..) with some lines open, so remap and mask them before
     * anything can fire.  Interrupts are still off here. */
    pic_remap();
    pic_mask_all();

    idt_load();
}

/* Called from isr_common for every vector.  The vector number was pushed by
 * the slot itself, so this is the one place that knows what fired. */
void isr_dispatch(isr_frame *f)
{
    com_print("isr vector=0x");
    com_hex(f->vector);
    com_print(" err=0x");
    com_hex(f->err);
    com_print(" eip=0x");
    com_hex(f->eip);
    com_print(" cs=0x");
    com_hex(f->cs);
    com_print("\r\n");
}
