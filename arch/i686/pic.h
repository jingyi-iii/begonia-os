/* pic.h - the 8259A interrupt controller pair, and the PIT behind IRQ0. */
#ifndef PIC_H
#define PIC_H

#include "typedefs.h"

void pic_remap(void);           /* IRQ0..15 -> vectors 0x20..0x2F            */
void pic_mask_all(void);        /* mask every line: nothing gets through     */
void pic_mask(u8 irq);          /* irq 0..15                                 */
void pic_unmask(u8 irq);        /* also unmasks the cascade for irq >= 8     */
void pic_eoi(u8 irq);           /* call at the end of every IRQ handler      */

void pit_init(u32 hz);          /* PIT channel 0 in mode 3                    */

#endif
