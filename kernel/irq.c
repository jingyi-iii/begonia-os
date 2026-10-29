#include "heap.h"
#include "interrupt.h"
#include "irq.h"
#include "log.h"
#include "string.h"

#define MAX_IRQ_LINE    (16)
static irqline* irqlines[MAX_IRQ_LINE] = {0};
static spinlock create_lock = SPINLOCK_INIT;

static inline int irqline_alloc(u32 major, irqline **out)
{
    if (!out)
        return E_INVAL;

    irqline *line = (irqline*)malloc(sizeof(irqline));
    if (!line)
        return E_NOMEM;

    line->major = major;
    spinlock_init(&line->splock);
    list_init(&line->irqs);

    *out = line;
    return 0;
}

static inline void irqline_free(irqline* line)
{
    if (!line)
        return;

    memset(line, 0, sizeof(irqline));
    free(line);
}

static void irqline_refresh_locked(struct irqline* line)
{
    int enable = 0;

    list_for_each(each, &line->irqs) {
        irq* p = list_entry(each, irq, node_in_irqline);
        if (p->enabled) {
            enable = 1;
            break;
        }
    }

    if (enable)
        interrupt_unmask(line->major);
    else
        interrupt_mask(line->major);
}

static inline int irqline_add_irq_locked(struct irqline* line, struct irq* p)
{
    if (!line || !p)
        return E_INVAL;

    list_add(&p->node_in_irqline, &line->irqs);
    return 0;
}

static inline int irqline_remove_irq_locked(struct irqline* line, struct irq* p)
{
    if (!line || !p)
        return E_INVAL;

    list_del(&p->node_in_irqline);
    return 0;
}

static inline int irqline_remove_all_locked(struct irqline* line)
{
    if (!line)
        return E_INVAL;

    list_for_each_safe(node, next, &line->irqs) {
        list_del(node);
    }
    return 0;
}

static int irqline_init(irqline** out_line, u32 major)
{
    if (!out_line || major >= MAX_IRQ_LINE)
        return E_INVAL;

    return irqline_alloc(major, out_line);
}

/*
 * Find the first unused minor number on the given irqline.
 * Returns E_NOTFOUND if none is available.
 */
static inline int irqline_get_available_minor_locked(struct irqline* line,
                                                     u32* minor)
{
    if (!minor)
        return E_INVAL;

    for (u32 candidate = 0; candidate < (UINT32_MAX - 1); candidate++) {
        int used = 0;
        list_for_each(each, &line->irqs) {
            irq* p = list_entry(each, irq, node_in_irqline);
            if (p->minor == candidate) {
                used = 1;
                break;
            }
        }
        if (!used) {
            *minor = candidate;
            return 0;
        }
    }

    return E_NOTFOUND;
}

void irqline_handler(u32 major, u32 minor, void* context)
{

}

static inline int irq_alloc(u32 major, u32 minor, const char *name,
              void *context, irq_handler_fn handler, irq **out)
{
    if (!out)
        return E_INVAL;

    irq *p = (irq*)malloc(sizeof(irq));
    if (!p)
        return E_NOMEM;

    p->name = name;
    p->context = context;
    p->major = major;
    p->minor = minor;
    p->handler = handler;
    p->enabled = 0;
    list_init(&p->node_in_irqline);
    *out = p;

    return 0;
}

static inline void irq_free(irq* p)
{
    if (!p)
        return;

    free(p);
}

int irq_request(irq **out, const char* name, u32 major,
                u32 minor, irq_handler_fn cb, void* cb_param)
{
    if (!out || major >= MAX_IRQ_LINE)
        return E_INVAL;

    int ret = 0;
    u32 eflags = 0;
    irqline* line = 0;

    eflags = spinlock_lock(&create_lock);
    if (!irqlines[major])
        ret = irqline_init(&irqlines[major], major);
    spinlock_unlock(&create_lock, eflags);

    if (ret) {
        LOG("irq_request: failed to init the irqline with major");
        return ret;
    }

    ret = irq_alloc(major, minor, name, cb_param, cb, out);
    if (ret) {
        LOG("irq_request: failed to alloc irq structure");
        return ret;
    }

    line = irqlines[major];
    eflags = spinlock_lock(&line->splock);

    if (minor == IRQ_ANY_MINOR) {
        ret = irqline_get_available_minor_locked(line, &minor);
        if (ret) {
            LOG("irq_request: no free minor on major");
            ret = E_IRQ_NOTAVAIL;
            goto failed;
        }
    }
    (*out)->minor = minor;

    list_for_each(each, &line->irqs) {
        irq* p = list_entry(each, irq, node_in_irqline);
        if (p->minor == minor) {
            LOG("irq_request: minor already exists");
            ret = E_IRQ_INUSE;
            goto failed;
        }
    }

    irqline_add_irq_locked(line, *out);
    ret = 0;
    goto exit;

failed:
    irq_free(*out);
    *out = 0;
exit:
    spinlock_unlock(&line->splock, eflags);
    return ret;
}

void irq_release(irq *p)
{
    if (!p || p->major >= MAX_IRQ_LINE)
        return;

    int empty = 0;
    irqline* line = irqlines[p->major];
    if (!line) {
        free(p);
        return;
    }

    u32 eflags = spinlock_lock(&line->splock);
    irqline_remove_irq_locked(line, p);
    irqline_refresh_locked(line);

    empty = list_empty(&line->irqs);
    spinlock_unlock(&line->splock, eflags);

    if (empty) {
        u32 flags = spinlock_lock(&create_lock);
        irqlines[p->major] = 0;
        spinlock_unlock(&create_lock, flags);
        irqline_free(line);
    }

    free(p);
}

int irq_mask(struct irq* p)
{
    if (!p || p->major >= MAX_IRQ_LINE)
        return E_INVAL;

    irqline* line = irqlines[p->major];
    if (!line)
        return E_INVAL;

    u32 eflags = spinlock_lock(&line->splock);
    p->enabled = 0;
    irqline_refresh_locked(line);
    spinlock_unlock(&line->splock, eflags);

    return 0;
}

int irq_unmask(struct irq* p)
{
    if (!p || p->major >= MAX_IRQ_LINE)
        return E_INVAL;

    irqline* line = irqlines[p->major];
    if (!line)
        return E_INVAL;

    u32 eflags = spinlock_lock(&line->splock);
    p->enabled = 1;
    irqline_refresh_locked(line);
    spinlock_unlock(&line->splock, eflags);

    return 0;
}