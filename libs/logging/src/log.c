#include "logging/log.h"

#include <stdarg.h>
#include <stdio.h>

void log_write(const char *level, const char *function, int line,
               const char *format, ...)
{
    printf("%s %s:%d ", level, function, line);
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
}
