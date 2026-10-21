#include "cpu.h"

u32 cpu_irq_save(void)
{
    u32 flags;

    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void cpu_irq_restore(u32 flags)
{
    __asm__ __volatile__("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

void cpu_relax(void)
{
    __asm__ __volatile__("pause");
}
