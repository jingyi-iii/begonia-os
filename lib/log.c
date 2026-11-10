/* log.c - the formatting behind LOG().  No libc here, so this is the small
 * subset the kernel actually uses: decimal, hex, binary, pointers, strings,
 * characters, plus width, precision and the '-'/'0' flags.  A line is
 * formatted into a caller-owned stack buffer, so a log call never shares
 * state with another one; only the console itself is shared, and com_print()
 * writes it byte by byte into a polled UART. */
#include <stdarg.h>

#include "com.h"
#include "log.h"
#include "typedefs.h"

#define LOG_LINE_MAX    (192u)

struct out {
    char* buf;
    u32   cap;
    u32   len;                  /* what the line wants, even when truncated */
};

static void out_putc(struct out* o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;

    o->len++;
}

static void out_fill(struct out* o, char c, u32 n)
{
    while (n--)
        out_putc(o, c);
}

static void out_str(struct out* o, const char* s, u32 width, i32 prec, int left)
{
    u32 len = 0;

    if (!s)
        s = "(null)";

    while (s[len])
        len++;
    if (prec >= 0 && len > (u32)prec)
        len = (u32)prec;

    if (!left)
        out_fill(o, ' ', width > len ? width - len : 0);

    for (u32 i = 0; i < len; i++)
        out_putc(o, s[i]);

    if (left)
        out_fill(o, ' ', width > len ? width - len : 0);
}

/* prec < 0 means none; for numbers it is the minimum number of digits, and
 * prec == 0 turns the value 0 into an empty string, exactly like printf. */
static void out_uint(struct out* o, u32 value, u32 base, int upper,
                     u32 width, i32 prec, char pad, int left)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";

    const char* digits = upper ? upper_digits : lower_digits;
    char tmp[33];               /* 32 binary digits and a terminator */
    u32 min_digits = (prec > 0) ? (u32)prec : 0;
    u32 n = 0;
    u32 ndigits;

    if (value == 0) {
        if (prec != 0)
            tmp[n++] = '0';
    } else {
        while (value) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }

    while (n < min_digits && n < sizeof tmp)
        tmp[n++] = '0';

    ndigits = n;

    if (!left)
        out_fill(o, pad, width > ndigits ? width - ndigits : 0);

    while (n)
        out_putc(o, tmp[--n]);

    if (left)
        out_fill(o, ' ', width > ndigits ? width - ndigits : 0);
}

static void out_int(struct out* o, i32 value, u32 width, i32 prec,
                    char pad, int left)
{
    u32 magnitude;

    if (value < 0) {
        out_putc(o, '-');
        magnitude = ~(u32)value + 1u;       /* two's complement, INT_MIN ok */
        if (width)
            width--;
    } else {
        magnitude = (u32)value;
    }

    out_uint(o, magnitude, 10, 0, width, prec, pad, left);
}

static void log_vformat(char* buf, u32 cap, const char* fmt, va_list ap)
{
    struct out o = { buf, cap, 0 };

    while (*fmt) {
        int left = 0;
        int zero = 0;
        u32 width = 0;
        i32 prec = -1;
        char pad;

        if (*fmt != '%') {
            out_putc(&o, *fmt++);
            continue;
        }
        fmt++;

        for (;;) {
            if (*fmt == '-') {
                left = 1;
                fmt++;
            } else if (*fmt == '0') {
                zero = 1;
                fmt++;
            } else {
                break;
            }
        }

        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 1000)
                width = width * 10 + (u32)(*fmt - '0');
            fmt++;
        }
        if (width >= cap)
            width = cap - 1;

        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                if (prec < 1000)
                    prec = prec * 10 + (*fmt - '0');
                fmt++;
            }
        }

        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h')
            fmt++;

        /* printf drops the zero flag once a precision is given */
        pad = (zero && !left && prec < 0) ? '0' : ' ';

        switch (*fmt) {
        case 'd':
        case 'i':
            out_int(&o, va_arg(ap, i32), width, prec, pad, left);
            break;
        case 'u':
            out_uint(&o, va_arg(ap, u32), 10, 0, width, prec, pad, left);
            break;
        case 'x':
            out_uint(&o, va_arg(ap, u32), 16, 0, width, prec, pad, left);
            break;
        case 'X':
            out_uint(&o, va_arg(ap, u32), 16, 1, width, prec, pad, left);
            break;
        case 'b':
            out_uint(&o, va_arg(ap, u32), 2, 0, width, prec, pad, left);
            break;
        case 'p':
            out_str(&o, "0x", 0, -1, 0);
            out_uint(&o, (u32)va_arg(ap, void*), 16, 0, 8, 8, '0', 0);
            break;
        case 'c':
            out_putc(&o, (char)va_arg(ap, int));
            break;
        case 's':
            out_str(&o, va_arg(ap, const char*), width, prec, left);
            break;
        case '%':
            out_putc(&o, '%');
            break;
        case '\0':
            fmt--;                          /* a trailing '%' */
            break;
        default:
            out_putc(&o, '%');
            out_putc(&o, *fmt);
            break;
        }

        if (*fmt)
            fmt++;
    }

    if (cap)
        buf[o.len < cap ? o.len : cap - 1] = '\0';
}

static int log_color_on = 1;

void log_set_color(int on)
{
    log_color_on = on;
}

int log_color_enabled(void)
{
    return log_color_on;
}

static const char* level_attr(log_level level)
{
    if (!log_color_on)
        return "";

    switch (level) {
    case LOG_OK:    return "\x1b[32m";
    case LOG_WARN:  return "\x1b[33m";
    case LOG_ERR:   return "\x1b[31m";
    case LOG_DEBUG: return "\x1b[36m";
    default:        return "";
    }
}

static const char* level_tag(log_level level)
{
    switch (level) {
    case LOG_OK:    return "[ok] ";
    case LOG_WARN:  return "[warn] ";
    case LOG_ERR:   return "[err] ";
    case LOG_DEBUG: return "[dbg] ";
    default:        return "";
    }
}

void log_print(log_level level, const char* fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list ap;
    u32 len = 0;
    const char* attr = level_attr(level);
    const char* tag = level_tag(level);

    va_start(ap, fmt);
    log_vformat(line, sizeof line, fmt, ap);
    va_end(ap);

    while (line[len])
        len++;
    if (len && line[len - 1] == '\n')
        line[len - 1] = '\0';               /* the CRLF below stands in for it */

    com_print(attr);
    com_print(tag);
    com_print(line);
    if (attr[0])
        com_print("\x1b[0m");               /* never let the colour leak */
    com_putc('\r');
    com_putc('\n');
}
