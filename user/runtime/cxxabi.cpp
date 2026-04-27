#include "syscall.h"

extern "C" {
void *__dso_handle = 0;
}

static void cxxabi_fatal(const char *message)
{
    sys_write(message);
    sys_exit(1);
    for (;;)
        ;
}

extern "C" void __cxa_pure_virtual(void)
{
    cxxabi_fatal("cxxabi: pure virtual function called\n");
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

extern "C" void __cxa_finalize(void *)
{
}
