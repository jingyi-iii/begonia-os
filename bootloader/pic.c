#include "pic.h"

#define PIC1_CMD        (0x20)
#define PIC1_DATA       (0x21)
#define PIC2_CMD        (0xA0)
#define PIC2_DATA       (0xA1)

#define PIC_EOI         (0x20)
#define ICW1_INIT       (0x10)
#define ICW1_ICW4       (0x01)

#define PIC_VECTOR_BASE (0x20)

#define PIT_CH0         (0x40)
#define PIT_CMD         (0x43)
#define PIT_HZ          (1193182u)   

void pic_remap(void)
{
    /* ICW1: initialise, ICW4 coming */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: vector base for each chip */
    outb(PIC1_DATA, PIC_VECTOR_BASE);
    io_wait();
    outb(PIC2_DATA, PIC_VECTOR_BASE + 8);
    io_wait();

    /* ICW3: how the two are wired together */
    outb(PIC1_DATA, 0x04);      /* master: slave sits on IRQ2                */
    io_wait();
    outb(PIC2_DATA, 0x02);      /* slave: its identity on the cascade        */
    io_wait();

    /* ICW4: 8086/8088 mode, normal EOI */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(u8 irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;

    outb(port, (u8)(inb(port) | (1u << (irq & 7))));
}

void pic_unmask(u8 irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;

    outb(port, (u8)(inb(port) & ~(1u << (irq & 7))));

    /* A line on the slave only reaches the CPU through IRQ2 on the master. */
    if (irq >= 8)
        outb(PIC1_DATA, (u8)(inb(PIC1_DATA) & ~(1u << 2)));
}

void pic_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);

    outb(PIC1_CMD, PIC_EOI);
}

void pit_init(u32 hz)
{
    u32 divisor = (hz == 0) ? 0xFFFFu : PIT_HZ / hz;

    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFFu)
        divisor = 0xFFFFu;

    outb(PIT_CMD, 0x36);        /* channel 0, lo/hi byte, mode 3, binary     */
    outb(PIT_CH0, (u8)(divisor & 0xFF));
    outb(PIT_CH0, (u8)(divisor >> 8));
}
