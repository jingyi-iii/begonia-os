#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "typedefs.h"

void interrupt_unmask(u16 irq_nr);
void interrupt_mask(u16 irq_nr);

#endif
