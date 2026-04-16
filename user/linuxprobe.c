/*
 * linuxprobe.c - static Linux i386 ABI probe built with i486-linux-musl-gcc.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

static size_t probe_strlen(const char *s)
{
    size_t n = 0;

    while (s && s[n])
        n++;
    return n;
}

static int fail(const char *msg)
{
    write(1, msg, probe_strlen(msg));
    return 1;
}

int main(void)
{
    struct utsname uts;
    struct timeval tv;
    struct stat st;
    char *heap;
    char *map;

    if (uname(&uts) != 0)
        return fail("linuxprobe: uname failed\n");
    if (strcmp(uts.sysname, "Drunix") != 0)
        return fail("linuxprobe: bad uname\n");
    if (gettimeofday(&tv, 0) != 0)
        return fail("linuxprobe: gettimeofday failed\n");
    if (fstat(1, &st) != 0)
        return fail("linuxprobe: fstat failed\n");

    heap = malloc(64);
    if (!heap)
        return fail("linuxprobe: malloc failed\n");
    heap[0] = 'h';
    heap[63] = '\0';

    map = mmap(0, 4096, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return fail("linuxprobe: mmap failed\n");
    map[0] = 'm';
    if (munmap(map, 4096) != 0)
        return fail("linuxprobe: munmap failed\n");

    write(1, "linuxprobe ok\n", 14);
    return 0;
}
