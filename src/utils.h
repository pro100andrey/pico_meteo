#ifndef _UTILS_H
#define _UTILS_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Allocate and format string. Allocates memory, caller must free.
char *new_string(const char *format, ...);

void custom_assert(int condition, const char *message, const char *file,
                   int line);

#define ASSERT(condition, message) custom_assert((condition), (message), __FILE__, __LINE__)

#endif
