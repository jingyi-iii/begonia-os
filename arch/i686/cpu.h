/* cpu.h - CPU state and the instructions that touch it.  The assembly lives
 * in cpu.c so that lib/ can stay free of it. */
#ifndef CPU_H
#define CPU_H

#include "typedefs.h"

static inline int is_user_mode(void)
{
    u16 cs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
    return (cs & 3) == 3;
}

/* Read EFLAGS with interrupts off, and hand the result back to
 * cpu_irq_restore().  The saved value is what makes nested critical sections
 * balance: an inner restore sees IF=0 and leaves it off, the outer one puts
 * back IF=1. */
u32  cpu_irq_save(void);
void cpu_irq_restore(u32 flags);

/* The hint that we are spinning.  An out-of-line call also stops the compiler
 * from hoisting the load out of the loop. */
void cpu_relax(void);

void cpu_halt(void);

#endif
