# Userland C++ Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add side-by-side C and C++ user program support with a repo-owned freestanding C++ runtime and a `/bin/cpphello` smoke binary.

**Architecture:** Keep the kernel, ELF loader, process entry stack, and existing C user programs unchanged. Add C++ support inside `user/` by extending the user linker script, startup path, libc headers, runtime objects, and Makefiles. Use `x86_64-elf-g++` for `.cpp` compilation, link explicitly with `x86_64-elf-ld`, and keep RTTI, exceptions, `libstdc++`, and `libsupc++` out of this milestone.

**Tech Stack:** Freestanding 32-bit x86 C/C++, NASM `crt0`, custom `user/user.ld`, existing Drunix user libc, Makefile build, DUFS disk image packaging, QEMU `make test-halt` verification.

---

## File Structure

- Modify `user/user.ld`: add constructor/destructor sections and exported section-boundary symbols while preserving the `0x400000` load address and BSS page alignment.
- Modify `user/lib/crt0.asm`: call C++ constructor/destructor runners around `main` without changing the process entry stack ABI.
- Create `user/lib/cxx_init.c`: iterate `.init_array`, `.ctors`, `.fini_array`, and `.dtors` entries.
- Create `user/lib/cxxrt.cpp`: provide `operator new`, `operator new[]`, `operator delete`, `operator delete[]`, and sized delete variants backed by `malloc`/`free`.
- Create `user/lib/cxxabi.cpp`: provide minimal ABI failure/finalization hooks such as `__cxa_pure_virtual`, `__cxa_atexit`, `__cxa_finalize`, and `__dso_handle`.
- Modify `user/lib/ctype.h`, `user/lib/malloc.h`, `user/lib/mman.h`, `user/lib/stdio.h`, `user/lib/stdlib.h`, `user/lib/string.h`, `user/lib/syscall.h`, `user/lib/time.h`, and `user/lib/unistd.h`: add C++ linkage guards so C++ user programs can include the C runtime headers.
- Create `user/cpphello.cpp`: C++ smoke user program covering global constructors/destructors, classes, virtual dispatch, `new`/`delete`, arrays, and calls into existing C libc/syscalls.
- Modify `user/Makefile`: add C++ compiler flags, runtime objects, a `.cpp` pattern rule, and a `cpphello` link target while preserving all C targets.
- Modify top-level `Makefile`: add `cpphello` to `USER_PROGS` so it is packaged as `/bin/cpphello`.
- Modify `.gitignore`: add `user/cpphello` as a generated binary path. Keep the user's current `docs/superpowers/` ignore removal intact if it is still present.
- Modify `README.md`: document mixed C/C++ userland support and the supported C++ subset.

## Scope Notes

The current `x86_64-elf-g++ -m32` output emits `.ctors` and `.dtors` for a global C++ object when compiled with `-fno-use-cxa-atexit`. The implementation must support `.ctors` and `.dtors` for this toolchain and also define `.init_array` and `.fini_array` boundaries so newer or different compiler output has a stable startup path.

The C++ flags for this milestone deliberately include `-fno-exceptions`, `-fno-rtti`, `-fno-use-cxa-atexit`, and `-fno-threadsafe-statics`. This gives classes, constructors, destructors, virtual dispatch, and allocation while avoiding the larger exception, RTTI, and guard-variable runtime.

## Task 1: C++ Linker Sections And Startup Hooks

**Files:**
- Modify: `user/user.ld`
- Modify: `user/lib/crt0.asm`
- Create: `user/lib/cxx_init.c`

- [ ] **Step 1: Run the current user build baseline**

Run:

```bash
make -C user clean
make -C user hello
```

Expected: `hello` builds with the existing C-only runtime. `git status --short` may show ignored build outputs, but no tracked source files should change from this step.

- [ ] **Step 2: Write a temporary failing constructor-section probe**

Run:

```bash
cat > /tmp/drunix-cxx-probe.cpp <<'EOF'
struct Probe {
    Probe();
    ~Probe();
    int value;
};

Probe::Probe() : value(7) {}
Probe::~Probe() { value = 0; }

Probe global_probe;

int main()
{
    return global_probe.value == 7 ? 0 : 1;
}
EOF
x86_64-elf-g++ -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
    -fno-stack-protector -fno-omit-frame-pointer -fno-exceptions -fno-rtti \
    -fno-use-cxa-atexit -fno-threadsafe-statics \
    -c /tmp/drunix-cxx-probe.cpp -o /tmp/drunix-cxx-probe.o
x86_64-elf-ld -m elf_i386 -T user/user.ld -o /tmp/drunix-cxx-probe \
    user/lib/crt0.o /tmp/drunix-cxx-probe.o user/lib/syscall.o
```

Expected: the final link fails because `_start` still requires C runtime symbols and there are no constructor/destructor runners. The probe also confirms the compiler emits constructor/destructor sections before the implementation changes.

- [ ] **Step 3: Extend the user linker script**

Edit `user/user.ld` so it matches this structure. Preserve the load address and BSS alignment:

```ld
/*
 * user.ld - linker script for ring-3 user programs.
 *
 * Loads the binary at virtual address 0x400000 (4 MB), which is well above
 * the kernel's 0-2 MB range and safely below the 3 GB user/kernel split.
 */

ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .init_array : ALIGN(4) {
        __init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP (*(.init_array))
        __init_array_end = .;
    }

    .ctors : ALIGN(4) {
        __ctors_start = .;
        KEEP (*(.ctors))
        KEEP (*(SORT_BY_NAME(.ctors.*)))
        __ctors_end = .;
    }

    .fini_array : ALIGN(4) {
        __fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP (*(.fini_array))
        __fini_array_end = .;
    }

    .dtors : ALIGN(4) {
        __dtors_start = .;
        KEEP (*(.dtors))
        KEEP (*(SORT_BY_NAME(.dtors.*)))
        __dtors_end = .;
    }

    .eh_frame : {
        *(.eh_frame)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
    }
}
```

- [ ] **Step 4: Add constructor/destructor runners**

Create `user/lib/cxx_init.c`:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */

typedef void (*cxx_init_fn_t)(void);

extern cxx_init_fn_t __init_array_start[];
extern cxx_init_fn_t __init_array_end[];
extern cxx_init_fn_t __ctors_start[];
extern cxx_init_fn_t __ctors_end[];
extern cxx_init_fn_t __fini_array_start[];
extern cxx_init_fn_t __fini_array_end[];
extern cxx_init_fn_t __dtors_start[];
extern cxx_init_fn_t __dtors_end[];

static void run_forward(cxx_init_fn_t *start, cxx_init_fn_t *end)
{
    while (start < end) {
        if (*start)
            (*start)();
        start++;
    }
}

static void run_reverse(cxx_init_fn_t *start, cxx_init_fn_t *end)
{
    while (end > start) {
        end--;
        if (*end)
            (*end)();
    }
}

void __drunix_run_constructors(void)
{
    run_forward(__init_array_start, __init_array_end);
    run_forward(__ctors_start, __ctors_end);
}

void __drunix_run_destructors(void)
{
    run_reverse(__fini_array_start, __fini_array_end);
    run_reverse(__dtors_start, __dtors_end);
}
```

- [ ] **Step 5: Call the runners from `_start`**

Edit `user/lib/crt0.asm` to add the two new external symbols and call them around `main`:

```asm
global _start
extern main
extern sys_exit
extern environ
extern __drunix_run_constructors
extern __drunix_run_destructors

section .text
_start:
    pop   eax        ; argc
    pop   ebx        ; argv (char **)
    pop   ecx        ; envp (char **)
    mov   [environ], ecx

    push  eax
    push  ebx
    push  ecx
    call  __drunix_run_constructors
    pop   ecx
    pop   ebx
    pop   eax

    push  ecx        ; main's third arg
    push  ebx        ; main's second arg
    push  eax        ; main's first arg
    call  main
    add   esp, 12    ; drop the three args we pushed

    push  eax        ; preserve main's return across destructor calls
    call  __drunix_run_destructors
    pop   eax

    push  eax        ; exit code = return value of main
    call  sys_exit
.hang:
    hlt
    jmp   .hang      ; unreachable - sys_exit never returns
```

- [ ] **Step 6: Wire the C startup object into the user runtime**

In `user/Makefile`, add `lib/cxx_init.o` to `LIB_OBJS` immediately after `lib/crt0.o`:

```make
LIB_OBJS = lib/crt0.o lib/cxx_init.o lib/syscall.o lib/malloc.o \
           lib/string.o lib/ctype.o lib/stdlib.o lib/stdio.o \
           lib/unistd.o lib/time.o
```

Add this compile rule after `lib/crt0.o`:

```make
lib/cxx_init.o: lib/cxx_init.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

- [ ] **Step 7: Verify existing C user programs still link**

Run:

```bash
make -C user clean
make -C user hello shell
```

Expected: `hello` and `shell` link successfully with `lib/cxx_init.o`. Existing C binaries should behave the same because empty constructor/destructor ranges are no-ops.

- [ ] **Step 8: Commit startup support**

Run:

```bash
git add user/user.ld user/lib/crt0.asm user/lib/cxx_init.c user/Makefile
git commit -m "feat: add userland C++ startup hooks"
```

## Task 2: C++-Safe User Runtime Headers

**Files:**
- Modify: `user/lib/ctype.h`
- Modify: `user/lib/malloc.h`
- Modify: `user/lib/mman.h`
- Modify: `user/lib/stdio.h`
- Modify: `user/lib/stdlib.h`
- Modify: `user/lib/string.h`
- Modify: `user/lib/syscall.h`
- Modify: `user/lib/time.h`
- Modify: `user/lib/unistd.h`

- [ ] **Step 1: Write a temporary failing C++ header probe**

Run:

```bash
cat > /tmp/drunix-header-probe.cpp <<'EOF'
#include "lib/ctype.h"
#include "lib/malloc.h"
#include "lib/mman.h"
#include "lib/stdio.h"
#include "lib/stdlib.h"
#include "lib/string.h"
#include "lib/syscall.h"
#include "lib/time.h"
#include "lib/unistd.h"

int main()
{
    char buf[8];
    memset(buf, 0, sizeof(buf));
    return isalpha('A') && strlen(buf) == 0 ? 0 : 1;
}
EOF
x86_64-elf-g++ -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
    -fno-stack-protector -fno-omit-frame-pointer -fno-exceptions -fno-rtti \
    -fno-use-cxa-atexit -fno-threadsafe-statics -I user \
    -c /tmp/drunix-header-probe.cpp -o /tmp/drunix-header-probe.o
```

Expected before this task's edits: compilation may succeed for some headers, but the headers do not yet guarantee C linkage for functions used by C++ programs.

- [ ] **Step 2: Add C++ linkage guards to each C header**

For each listed header, add this block after includes and type definitions that the prototypes need:

```c
#ifdef __cplusplus
extern "C" {
#endif
```

Add this block immediately before the header's final `#endif`:

```c
#ifdef __cplusplus
}
#endif
```

Apply the pattern to:

```text
user/lib/ctype.h
user/lib/malloc.h
user/lib/mman.h
user/lib/stdio.h
user/lib/stdlib.h
user/lib/string.h
user/lib/syscall.h
user/lib/time.h
user/lib/unistd.h
```

For `user/lib/syscall.h`, place the opening block after the `dufs_stat_t`, `termios_t`, and `sys_timespec_t` type definitions and memory-mapping macros, before the first function prototype `void sys_exit(int code);`.

For `user/lib/stdio.h`, place the opening block after the `FILE` typedef and the `stdin`, `stdout`, and `stderr` declarations are still inside the linkage block.

- [ ] **Step 3: Verify the C++ header probe compiles**

Run:

```bash
x86_64-elf-g++ -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
    -fno-stack-protector -fno-omit-frame-pointer -fno-exceptions -fno-rtti \
    -fno-use-cxa-atexit -fno-threadsafe-statics -I user \
    -c /tmp/drunix-header-probe.cpp -o /tmp/drunix-header-probe.o
```

Expected: compilation succeeds.

- [ ] **Step 4: Verify C headers still compile from C**

Run:

```bash
make -C user clean
make -C user hello shell
```

Expected: both C user programs build successfully.

- [ ] **Step 5: Commit header compatibility**

Run:

```bash
git add user/lib/ctype.h user/lib/malloc.h user/lib/mman.h user/lib/stdio.h \
    user/lib/stdlib.h user/lib/string.h user/lib/syscall.h user/lib/time.h \
    user/lib/unistd.h
git commit -m "feat: make user libc headers C++ compatible"
```

## Task 3: Minimal Userland C++ Runtime

**Files:**
- Create: `user/lib/cxxrt.cpp`
- Create: `user/lib/cxxabi.cpp`
- Modify: `user/Makefile`

- [ ] **Step 1: Write a temporary failing runtime probe**

Run:

```bash
cat > /tmp/drunix-runtime-probe.cpp <<'EOF'
#include "lib/stdio.h"

class Base {
public:
    virtual const char *message() const = 0;
    virtual ~Base() {}
};

class Derived : public Base {
public:
    const char *message() const override { return "ok"; }
};

int main()
{
    Base *b = new Derived();
    const char *msg = b->message();
    delete b;
    printf("%s\n", msg);
    return 0;
}
EOF
x86_64-elf-g++ -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
    -fno-stack-protector -fno-omit-frame-pointer -fno-exceptions -fno-rtti \
    -fno-use-cxa-atexit -fno-threadsafe-statics -I user \
    -c /tmp/drunix-runtime-probe.cpp -o /tmp/drunix-runtime-probe.o
x86_64-elf-ld -m elf_i386 -T user/user.ld -o /tmp/drunix-runtime-probe \
    user/lib/crt0.o user/lib/cxx_init.o user/lib/syscall.o user/lib/malloc.o \
    user/lib/string.o user/lib/ctype.o user/lib/stdlib.o user/lib/stdio.o \
    user/lib/unistd.o user/lib/time.o /tmp/drunix-runtime-probe.o
```

Expected: link fails with missing C++ runtime symbols such as `operator new`, `operator delete`, and pure virtual support.

- [ ] **Step 2: Add allocation operators**

Create `user/lib/cxxrt.cpp`:

```cpp
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "malloc.h"
#include "syscall.h"

#include <stddef.h>

static void cxx_fatal(const char *msg)
{
    sys_write(msg);
    sys_exit(1);
    for (;;) { }
}

void *operator new(size_t size)
{
    if (size == 0)
        size = 1;
    void *ptr = malloc(size);
    if (!ptr)
        cxx_fatal("c++ runtime: operator new out of memory\n");
    return ptr;
}

void *operator new[](size_t size)
{
    if (size == 0)
        size = 1;
    void *ptr = malloc(size);
    if (!ptr)
        cxx_fatal("c++ runtime: operator new[] out of memory\n");
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
```

- [ ] **Step 3: Add minimal ABI hooks**

Create `user/lib/cxxabi.cpp`:

```cpp
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall.h"

extern "C" void *__dso_handle = 0;

static void cxxabi_fatal(const char *msg)
{
    sys_write(msg);
    sys_exit(1);
    for (;;) { }
}

extern "C" void __cxa_pure_virtual(void)
{
    cxxabi_fatal("c++ runtime: pure virtual call\n");
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

extern "C" void __cxa_finalize(void *)
{
}
```

- [ ] **Step 4: Add C++ runtime objects and compile rules**

In `user/Makefile`, add these variables after `CFLAGS`:

```make
CXX    = x86_64-elf-g++
CXXFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
           -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
           -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
           -fno-threadsafe-statics \
           -fdebug-prefix-map=$(abspath ..)=.
CXXLIBS := $(shell $(CXX) -m32 -print-libgcc-file-name)
```

Add a separate C++ runtime object list after `LIB_OBJS`:

```make
CXX_RUNTIME_OBJS = lib/cxxrt.o lib/cxxabi.o
```

Add explicit compile rules near the other `lib/*.o` rules:

```make
lib/cxxrt.o: lib/cxxrt.cpp lib/malloc.h lib/syscall.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

lib/cxxabi.o: lib/cxxabi.cpp lib/syscall.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<
```

Add a `.cpp` program-object pattern rule after the existing `.c` rule:

```make
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I . -c -o $@ $<
```

- [ ] **Step 5: Verify the runtime probe links with explicit runtime objects**

Run:

```bash
make -C user lib/cxxrt.o lib/cxxabi.o
x86_64-elf-ld -m elf_i386 -T user/user.ld -o /tmp/drunix-runtime-probe \
    user/lib/crt0.o user/lib/cxx_init.o user/lib/syscall.o user/lib/malloc.o \
    user/lib/string.o user/lib/ctype.o user/lib/stdlib.o user/lib/stdio.o \
    user/lib/unistd.o user/lib/time.o user/lib/cxxrt.o user/lib/cxxabi.o \
    /tmp/drunix-runtime-probe.o \
    "$(x86_64-elf-g++ -m32 -print-libgcc-file-name)"
x86_64-elf-nm -u /tmp/drunix-runtime-probe
```

Expected: the link succeeds. `x86_64-elf-nm -u` prints no unresolved symbols.

- [ ] **Step 6: Commit C++ runtime support**

Run:

```bash
git add user/lib/cxxrt.cpp user/lib/cxxabi.cpp user/Makefile
git commit -m "feat: add userland C++ runtime objects"
```

## Task 4: C++ Smoke Program And Build Integration

**Files:**
- Create: `user/cpphello.cpp`
- Modify: `user/Makefile`
- Modify: `Makefile`
- Modify: `.gitignore`

- [ ] **Step 1: Add the smoke program**

Create `user/cpphello.cpp`:

```cpp
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * cpphello.cpp - C++ smoke test for the Drunix ring-3 user runtime.
 */

#include "lib/stdio.h"
#include "lib/syscall.h"

static int g_ctor_value = 0;

class GlobalProbe {
public:
    GlobalProbe()
    {
        g_ctor_value = 42;
    }

    ~GlobalProbe()
    {
        sys_write("cpphello: global destructor ran\n");
    }
};

static GlobalProbe g_probe;

class Greeter {
public:
    virtual const char *message() const
    {
        return "base";
    }

    virtual ~Greeter()
    {
    }
};

class DrunixGreeter : public Greeter {
public:
    const char *message() const override
    {
        return "virtual dispatch works";
    }
};

static int sum_array(const int *values, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
        total += values[i];
    return total;
}

int main(int argc, char **argv)
{
    printf("Hello from C++ userland!\n");
    printf("argc=%d\n", argc);
    if (argc > 0)
        printf("argv[0]=%s\n", argv[0]);
    printf("global constructor value=%d\n", g_ctor_value);

    Greeter *greeter = new DrunixGreeter();
    printf("%s\n", greeter->message());
    delete greeter;

    int *values = new int[3];
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    int total = sum_array(values, 3);
    delete[] values;

    printf("new[] sum=%d\n", total);

    if (g_ctor_value != 42)
        return 1;
    if (total != 6)
        return 2;
    return 0;
}
```

- [ ] **Step 2: Add the user Makefile target**

In `user/Makefile`, append `cpphello` to `PROGS`:

```make
PROGS = hello shell writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello
```

Add this link target near the other program targets:

```make
cpphello: cpphello.o $(LIB_OBJS) $(CXX_RUNTIME_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) $(CXX_RUNTIME_OBJS) cpphello.o $(CXXLIBS)
```

- [ ] **Step 3: Add top-level disk packaging**

In the top-level `Makefile`, append `cpphello` to `USER_PROGS`:

```make
USER_PROGS    := shell hello writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello
```

- [ ] **Step 4: Ignore the generated C++ binary**

In `.gitignore`, add the generated binary path next to the other `user/*` binaries:

```gitignore
user/cpphello
```

Do not re-add `docs/superpowers/` to `.gitignore` if the working tree still has that ignore entry removed.

- [ ] **Step 5: Verify the smoke program builds**

Run:

```bash
make -C user clean
make -C user cpphello
x86_64-elf-nm -u user/cpphello
```

Expected: `cpphello` builds. `x86_64-elf-nm -u user/cpphello` prints no unresolved symbols.

- [ ] **Step 6: Verify existing C programs still build**

Run:

```bash
make -C user all
```

Expected: all C user programs and `cpphello` build successfully.

- [ ] **Step 7: Commit smoke program integration**

Run:

```bash
git add user/cpphello.cpp user/Makefile Makefile .gitignore
git commit -m "feat: add C++ user smoke program"
```

## Task 5: Documentation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update dependencies**

In `README.md`, update the required dependency list so the cross C++ compiler is explicit:

```markdown
- `x86_64-elf-gcc`
- `x86_64-elf-g++`
- `x86_64-elf-ld`
```

In the macOS install command, keep the existing Homebrew package but add the verification command below the install block:

```sh
x86_64-elf-g++ --version
```

In the Windows/Linux cross-toolchain verification block, add:

```sh
x86_64-elf-g++ --version
```

- [ ] **Step 2: Document the C++ userland subset**

Add this subsection under `### Build Options` after the `MOUSE_SPEED` description:

```markdown
### Userland C++ Support

User programs can be written in C or in a freestanding C++ subset. C programs
continue to compile with `x86_64-elf-gcc`; C++ programs compile with
`x86_64-elf-g++` and link against the repo-owned user runtime in `user/lib`.

The current C++ userland supports global constructors and destructors,
classes, virtual dispatch, `new`, `delete`, `new[]`, and `delete[]`.
Allocation uses the existing `malloc` and `free` implementation backed by
`SYS_BRK`.

Exceptions, RTTI, `libstdc++`, and `libsupc++` are not part of the current
runtime. Code that depends on those features should fail at compile or link
time instead of pulling in hosted runtime libraries implicitly.

The smoke binary is `/bin/cpphello`, built from `user/cpphello.cpp`.
```

- [ ] **Step 3: Verify the README change is limited**

Run:

```bash
git diff -- README.md
```

Expected: the diff only documents `x86_64-elf-g++` and the supported userland C++ subset.

- [ ] **Step 4: Commit documentation**

Run:

```bash
git add README.md
git commit -m "docs: document userland C++ support"
```

## Task 6: Final Verification And Push

**Files:**
- No source edits unless a verification command exposes a bug.

- [ ] **Step 1: Confirm the branch and working tree**

Run:

```bash
git branch --show-current
git status --short
```

Expected: current branch is `feature/cpp-conversion`. The status is clean before final verification, except for build artifacts ignored by `.gitignore`.

- [ ] **Step 2: Run the userland build**

Run:

```bash
make -C user clean
make -C user all
```

Expected: all user programs, including `cpphello`, build successfully.

- [ ] **Step 3: Verify `cpphello` has no unresolved symbols**

Run:

```bash
x86_64-elf-nm -u user/cpphello
```

Expected: no output.

- [ ] **Step 4: Build the DUFS disk image**

Run:

```bash
make disk
```

Expected output includes a `/bin/cpphello` entry from `tools/mkfs.py`.

- [ ] **Step 5: Run the headless kernel baseline**

Run:

```bash
make test-halt
```

Expected: the target exits successfully after finding both double-fault log markers in `debugcon-df.log`.

- [ ] **Step 6: Check for forbidden hosted runtime dependencies**

Run:

```bash
x86_64-elf-nm user/cpphello | rg "libstdc|libsupc|__gxx_personality|_Unwind|typeinfo|__cxa_throw|__cxa_begin_catch"
```

Expected: no matches. `rg` exits with status 1 when there are no matches; that is the desired result for this command.

- [ ] **Step 7: Push the completed branch to origin**

Run this only after all verification commands above pass:

```bash
git status --short
git push -u origin feature/cpp-conversion
```

Expected: the branch is pushed to `origin/feature/cpp-conversion` so it can be tested on another machine.
