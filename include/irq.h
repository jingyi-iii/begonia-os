#ifndef IRQ_H
#define IRQ_H

#include "errno.h"
#include "list.h"
#include "spinlock.h"
#include "typedefs.h"

#define IRQ_ANY_MINOR   (0xFFFFFFFFu)
#define UINT32_MAX      (4294967295U)

typedef void (*irq_handler_fn)(void* context);

typedef struct irq {
    const char*     name;
    void*           context;
    u32             major;
    u32             minor;
    int             enabled;
    list_node       node_in_irqline;
    irq_handler_fn  handler;
} irq;

typedef struct irqline {
    u32             major;
    spinlock        splock;
    list_node       irqs;
} irqline;

int irq_request(irq **out, const char* name, u32 major,
                u32 minor, irq_handler_fn cb, void* cb_param);
void irq_release(irq *p);
int irq_mask(struct irq* p);
int irq_unmask(struct irq* p);


#endif
