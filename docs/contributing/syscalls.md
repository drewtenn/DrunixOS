# Syscall Table Maintenance

Whenever `kernel/proc/syscall.h` or `user/runtime/syscall.c` is created or modified, keep the syscall number table in the block comment at the top of `user/runtime/syscall.c` in sync with the `#define SYS_*` constants in `kernel/proc/syscall.h`.

Every syscall number, name, and argument summary must match.
