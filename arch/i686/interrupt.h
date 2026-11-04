#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "typedefs.h"

/* What isr_common in interrupt.S leaves on the stack.  Keep in step
 * with the layout comment at the top of that file. */
typedef struct {
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha                  */
    u32 gs, fs, es, ds;                            /* isr_common             */
    u32 vector, err;                              /* slot, then CPU or 0    */
    u32 eip, cs, eflags;                          /* pushed by the CPU      */
    u32 esp_user, ss_user;                        /* only when cs & 3 == 3   */
} isr_frame;

_Static_assert(sizeof(isr_frame) == 76, "isr_frame must match interrupt.S");

/* True when the trap came from ring 3, i.e. the user esp/ss above are valid. */
static inline int isr_from_user(const isr_frame* f)
{
    return (f->cs & 3) == 3;
}

void interrupt_unmask(u16 irq_nr);
void interrupt_mask(u16 irq_nr);
int syscall(u32 id, void* data, u32 data_size);

#endif
