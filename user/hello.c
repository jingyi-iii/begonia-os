/*
 * user/hello.c - the smallest ring-3 program that proves the ELF loader works.
 *
 * It is linked at the base of the user window and has no kernel symbols at
 * all: output goes straight to COM1 with port I/O (the ring-3 iret frame sets
 * IOPL=3), and the program never returns - the timer preempts it.
 */

#include "typedefs.h"

#define COM1_BASE   0x3F8
#define COM1_THR    (COM1_BASE + 0)     /* transmit holding register */
#define COM1_LSR    (COM1_BASE + 5)     /* line status register      */
#define LSR_THRE    (1 << 5)            /* transmitter holding empty  */

static inline void outb(u16 port, u8 value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 value;

    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

static void putc(char c)
{
    while (!(inb(COM1_LSR) & LSR_THRE))
        ;

    outb(COM1_THR, (u8)c);
}

static void puts(const char* s)
{
    while (*s)
        putc(*s++);
}

void _start(void)
{
    puts("[hello] running in ring 3, loaded from hello.elf\r\n");

    for (;;)
        ;
}
