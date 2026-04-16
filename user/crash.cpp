/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * crash.c — deliberately trigger a CPU exception to verify that the kernel
 * delivers a signal to the faulting process without crashing the whole OS.
 *
 * Usage: crash <type>
 *
 *   divzero  — integer divide by zero     → #DE (vector 0)  → SIGFPE
 *   nullptr  — null pointer dereference   → #PF (vector 14) → SIGSEGV
 *   badptr   — write to unmapped address  → #PF (vector 14) → SIGSEGV
 *   ud2      — invalid opcode (UD2)       → #UD (vector 6)  → SIGILL
 *   gpfault  — ring-3 #GP via bad segment → #GP (vector 13) → SIGSEGV
 *
 * After each test you should see the shell prompt return — the OS must remain
 * running.  If the screen prints "[ISR] --- HALTED ---" the kernel mistakenly
 * treated the fault as a ring-0 exception; if the machine reboots or freezes
 * the double-fault handler is broken.
 */

#include "lib/stdio.h"
#include "lib/string.h"

/* Prevent the compiler from optimising away the faulting operations. */
static volatile int zero = 0;
static volatile int result;

static void do_divzero(void)
{
    printf("crash: dividing by zero...\n");
    result = 42 / zero;   /* #DE: divide by zero */
    printf("crash: ERROR — should not reach here\n");
}

static void do_nullptr(void)
{
    volatile int *p = (volatile int *)0;
    printf("crash: dereferencing NULL pointer...\n");
    result = *p;          /* #PF: read from address 0x00000000 */
    printf("crash: ERROR — should not reach here\n");
}

static void do_badptr(void)
{
    volatile int *p = (volatile int *)0xDEADBEEF;
    printf("crash: writing to unmapped address 0xDEADBEEF...\n");
    *p = 0xBAD;           /* #PF: write to unmapped address */
    printf("crash: ERROR — should not reach here\n");
}

static void do_ud2(void)
{
    printf("crash: executing invalid opcode (UD2)...\n");
    __asm__ volatile ("ud2");   /* #UD: guaranteed invalid opcode */
    printf("crash: ERROR — should not reach here\n");
}

static void do_gpfault(void)
{
    printf("crash: loading null selector into %%ds to force #GP...\n");
    /*
     * Loading selector 0x0007 into %ds: index=0 (null descriptor), RPL=3.
     * The CPU raises #GP because the null descriptor has no valid base/limit.
     */
    __asm__ volatile (
        "mov $0x0007, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        : : : "eax"
    );
    printf("crash: ERROR — should not reach here\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: crash <type>\n"
            "types: divzero  nullptr  badptr  ud2  gpfault\n");
        return 1;
    }

    const char *type = argv[1];

    if (strcmp(type, "divzero") == 0)  { do_divzero();  return 0; }
    if (strcmp(type, "nullptr") == 0)  { do_nullptr();  return 0; }
    if (strcmp(type, "badptr")  == 0)  { do_badptr();   return 0; }
    if (strcmp(type, "ud2")     == 0)  { do_ud2();      return 0; }
    if (strcmp(type, "gpfault") == 0)  { do_gpfault();  return 0; }

    fprintf(stderr, "crash: unknown type '%s'\n", type);
    fprintf(stderr, "types: divzero  nullptr  badptr  ud2  gpfault\n");
    return 1;
}
