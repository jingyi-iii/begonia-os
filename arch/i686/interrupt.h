#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "typedefs.h"

/* What isr_common in interrupt.S leaves on the stack.  Keep in step
 * with the layout comment at the top of that file. */
typedef struct {
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha                  */
    u32 vector, err;                              /* slot, then CPU or 0    */
    u32 eip, cs, eflags;                          /* pushed by the CPU      */
} isr_frame;

_Static_assert(sizeof(isr_frame) == 52, "isr_frame must match interrupt.S");

void interrupt_unmask(u16 irq_nr);
void interrupt_mask(u16 irq_nr);

#endif
