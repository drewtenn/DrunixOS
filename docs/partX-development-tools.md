# Part X — Development Tools and Language Support

Once processes can crash, the kernel needs a way to capture and analyze the crash state. Once userland can run C programs, developers want to write C++ too. Part X covers both.

By the end of Part IX, Drunix has crossed from a text-first teaching kernel into a small interactive desktop OS. That gives us something more interesting to build on, but it also raises the bar for development work. Once a system has processes, files, a shell, memory protection, a desktop, and enough userland to run real programs, the next questions become practical ones: how do we debug it when it breaks, and how do we expand the kinds of programs it can run without weakening the boundary between the kernel and user space?

The shared thread is that both topics live on the developer's side of the machine. Kernel debugging is what we reach for when the system itself misbehaves, and C++ support is what we reach for when we want richer user programs to work with. Neither is a kernel feature the end user sees directly; both are tools that make Drunix more tractable to build on.

This part answers those questions from the developer's side of the machine. Chapter 29 makes kernel debugging reliable by separating early exception setup from the later moment when hardware interrupts are enabled, then ties live GDB sessions together with serial logs, debugcon output (QEMU's simple debug-console output port), and process core dumps. Chapter 30 returns to the user runtime and adds freestanding C++ support for ring-3 programs: constructor and destructor execution, minimal C++ ABI (Application Binary Interface) glue, allocation operators backed by the existing heap, and an explicit build contract that keeps hosted C++ libraries out of the image.

By the end of Part X, Drunix is easier to investigate and more flexible to program. The machine can be stopped, inspected, and understood when something goes wrong, and its userland can host both C programs and a carefully bounded C++ subset without asking the kernel to know anything about the source language that produced an executable.
