/* log.h - the kernel's one logging macro.  It is deliberately thin: there is
 * no printf yet, so a message is a fixed string. */
#ifndef LOG_H
#define LOG_H

#include "com.h"

#define LOG(msg)        do { com_print(msg); com_print("\r\n"); } while (0)

#endif
