/* log.h - the kernel's one logging entry point: printf-lite to COM1. */
#ifndef LOG_H
#define LOG_H

typedef enum {
    LOG_INFO = 0,               /* plain text, no tag                      */
    LOG_OK,                     /* green                                   */
    LOG_WARN,                   /* yellow                                  */
    LOG_ERR,                    /* red                                     */
    LOG_DEBUG,                  /* cyan                                    */
} log_level;

/* Format a line and write it to the serial console.
 *
 *   %d %i   signed int          %u      unsigned int
 *   %x %X   hex (lower/upper)   %b      binary
 *   %p      pointer as 0x%08x   %c      one character
 *   %s      NUL-terminated string        %%  a literal percent
 *
 * Flags, width and precision work the way printf's do: %08x, %5d,
 * %-5s, %.3s, %.4x.  A length modifier (l, z, h) is accepted and
 * ignored: every integer in this kernel is 32 bits wide.
 *
 * Levels below WARN add a short tag so a saved log keeps its meaning when
 * the colours are turned off; INFO stays plain, which is what the kernel
 * printed before levels existed. */
void log_print(log_level level, const char* fmt, ...);

/* Colour is on unless it is turned off; use that when the output is going to
 * a file rather than to a terminal. */
void log_set_color(int on);
int  log_color_enabled(void);

/* The plain one, kept at its old name so existing call sites are untouched. */
#define LOG(...)            log_print(LOG_INFO, __VA_ARGS__)
#define log(...)            log_print(LOG_INFO, __VA_ARGS__)
#define log_success(...)    log_print(LOG_OK, __VA_ARGS__)
#define log_warn(...)       log_print(LOG_WARN, __VA_ARGS__)
#define log_err(...)        log_print(LOG_ERR, __VA_ARGS__)
#define log_error(...)      log_print(LOG_ERR, __VA_ARGS__)
#define log_debug(...)      log_print(LOG_DEBUG, __VA_ARGS__)

#endif
