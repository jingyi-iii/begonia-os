/* com.h - the COM1 serial console. */
#ifndef COM_H
#define COM_H

#include "boot_common.h"

void com_init(void);            /* 115200 8N1, FIFOs on: call once, early     */
void com_putc(u8 c);            /* one byte, spins until the transmitter frees */
void com_print(const char *s);  /* NUL-terminated string                      */
void com_hex(u32 value);        /* 8 hex digits, no prefix                    */

#endif
