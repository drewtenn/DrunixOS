#ifndef MMAN_H
#define MMAN_H

#include "syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void *mmap(void *addr, unsigned int length, int prot, int flags,
                         int fd, unsigned int offset)
{
    return sys_mmap(addr, length, prot, flags, fd, offset);
}

static inline int munmap(void *addr, unsigned int length)
{
    return sys_munmap(addr, length);
}

static inline int mprotect(void *addr, unsigned int length, int prot)
{
    return sys_mprotect(addr, length, prot);
}

#ifdef __cplusplus
}
#endif

#endif
