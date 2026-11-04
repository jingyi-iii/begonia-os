/* log.h - the kernel's one logging entry point: printf-lite to COM1. */
#ifndef LOG_H
#define LOG_H

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
 * A trailing newline in the format is redundant; the line ends with CRLF
 * either way. */
void log_write(const char* fmt, ...);

#define LOG(...)        log_write(__VA_ARGS__)

#endif
