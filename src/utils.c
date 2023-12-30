#include "utils.h"

char *new_string(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    char *str = (char *)malloc(len + 1);
    if (!str)
    {
        return NULL;
    }

    va_start(args, format);
    vsnprintf(str, len + 1, format, args);
    va_end(args);

    return str;
}

void custom_assert(int condition, const char *message, const char *file, int line)
{
    if (!condition)
    {
        fprintf(stderr, "Assertion failed: %s, file %s, line %d\n", message, file, line);
        abort();
    }
}

#define ASSERT(condition, message) custom_assert((condition), (message), __FILE__, __LINE__)

