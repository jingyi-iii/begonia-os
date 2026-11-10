/* log_colors.c - shows what LOG() and its levelled siblings look like.
 *
 * Built by `make demo` into build/demo.elf and booted by `make run-demo`:
 * the demo supplies kmain, so it links against every kernel object except
 * kernel.o. */
#include "boot.h"
#include "errno.h"
#include "irq.h"
#include "log.h"
#include "process.h"
#include "typedefs.h"

static void demo_irq(void* context)
{
    (void)context;
}

void kmain(void)
{
    LOG("=== levels, colour on ===");
    log("info    plain text: no tag, no colour");
    log_success("ok      this line is green");
    log_warn("warn    this line is yellow");
    log_err("err     this line is red");
    log_error("error   the same thing under its long name");
    log_debug("debug   this line is cyan");

    LOG("");
    LOG("=== format specifiers ===");
    log("decimal  %d %d", -2147483647 - 1, 42);
    log("unsigned %u", 4294967295u);
    log("hex      %x  %X  %08x", 0xdeadbeef, 0xdeadbeef, 0x42);
    log("binary   %b   (0xa5)", 0xa5);
    log("pointer  %p", (void*)&kmain);
    log("string   %s", "hello");
    log("char     %c", '!');
    log("width    [%5d] [%-5d] [%05d]", 42, 42, 42);
    log("precision [%.3s] [%.5d]", "abcdef", 42);
    log("errno    %s (%d)", strerror(E_IRQ_INUSE), E_IRQ_INUSE);
    log("percent  100%% done");

    LOG("");
    LOG("=== real kernel error paths ===");
    {
        irq* first = 0;
        irq* second = 0;

        (void)irq_request(&first, "demo", 0, 0, demo_irq, 0);
        /* the same line and minor again: irq_request says so in yellow */
        (void)irq_request(&second, "demo", 0, 0, demo_irq, 0);
    }
    /* a tid nobody owns: thread_block warns in yellow */
    thread_block(9999);

    LOG("");
    LOG("=== colour off: the tags carry the level ===");
    log_set_color(0);
    log("info    plain");
    log_success("ok      tag only");
    log_warn("warn    tag only");
    log_err("err     tag only");
    log_debug("debug   tag only");
    log_set_color(1);

    LOG("");
    log_success("demo finished");

    exit_from_qemu();
}
