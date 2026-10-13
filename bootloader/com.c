#include "com.h"

/* COM1 at 0x3F8, the usual 16550 register block.  Only a few of them matter
 * here, and LSR is the one to poll before every byte: reading the data port
 * (0x3F8) for "transmitter empty" never succeeds and hangs the caller. */
#define COM1_THR  0x3F8     /* transmit holding register, write      */
#define COM1_DLL  0x3F8     /* divisor latch low,  while DLAB is set */
#define COM1_DLM  0x3F9     /* divisor latch high, while DLAB is set */
#define COM1_FCR  0x3FA     /* FIFO control, write                   */
#define COM1_LCR  0x3FB     /* line control                          */
#define COM1_LSR  0x3FD     /* line status: bit 5 = transmitter free */


/* 115200 8N1, FIFOs on.  The 16550 is clocked at 1.8432 MHz and divides that
 * by 16 and then by the divisor: 1 gives 115200, 12 would give 9600. */
void com_init(void)
{
    outb(COM1_LCR, 0x80);   /* DLAB on: 0x3F8/0x3F9 become the divisor      */
    outb(COM1_DLL, 0x01);   /* divisor 1                                    */
    outb(COM1_DLM, 0x00);
    outb(COM1_LCR, 0x03);   /* DLAB off, 8 data bits, no parity, 1 stop bit */
    outb(COM1_FCR, 0xC7);   /* FIFOs on and cleared, 14-byte trigger        */
}

/* One byte, waiting for the transmitter.  Everything that prints goes through
 * here, so no two messages can interleave. */
void com_putc(u8 c)
{
    while (!(inb(COM1_LSR) & 0x20))
        ;

    outb(COM1_THR, c);
}

void com_print(const char *s)
{
    while (*s)
        com_putc((u8)*s++);
}

void com_hex(u32 value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4)
        com_putc((u8)digits[(value >> shift) & 0xF]);
}
