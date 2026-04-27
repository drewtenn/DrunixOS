#include "malloc.h"
#include "syscall.h"
#include <stddef.h>

static void cxx_fatal(const char *message)
{
    sys_write(message);
    sys_exit(1);
    for (;;)
        ;
}

void *operator new(size_t size)
{
    if (size == 0)
        size = 1;

    void *ptr = malloc(size);
    if (!ptr)
        cxx_fatal("cxxrt: operator new failed\n");
    return ptr;
}

void *operator new[](size_t size)
{
    if (size == 0)
        size = 1;

    void *ptr = malloc(size);
    if (!ptr)
        cxx_fatal("cxxrt: operator new[] failed\n");
    return ptr;
}

void operator delete(void *ptr) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr) noexcept
{
    free(ptr);
}

void operator delete(void *ptr, size_t) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept
{
    free(ptr);
}
