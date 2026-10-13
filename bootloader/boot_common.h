#ifndef BOOT_COMMON_H
#define BOOT_COMMON_H

#define u8  unsigned char
#define u16 unsigned short
#define u32 unsigned int
#define u64 unsigned long long

/* Port I/O, shared by the drivers that need it (com.c, pic.c). */
static inline void outb(u16 port, u8 value)
{
    __asm__ __volatile__ ("outb %0, %1" : : "a"(value), "d"(port));
}

static inline u8 inb(u16 port)
{
    u8 value;

    __asm__ __volatile__ ("inb %1, %0" : "=a"(value) : "d"(port));
    return value;
}

/* A write to the unused port 0x80: the classic way to give slow chips
 * such as the 8259s time to react. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
