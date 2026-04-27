KTEST_SHARED_OBJS = kernel/test/ktest.o \
                    kernel/test/test_arch_shared.o \
                    kernel/test/test_console_runtime.o \
                    kernel/test/test_console_terminal.o \
                    kernel/test/test_pty.o \
                    kernel/test/test_pmm_core.o \
                    kernel/test/test_kheap.o \
                    kernel/test/test_vfs.o \
                    kernel/test/test_sched.o \
                    kernel/test/test_fs.o \
                    kernel/test/test_blkdev.o

KTEST_X86_OBJS = kernel/arch/x86/test/test_pmm.o \
                 kernel/arch/x86/test/test_arch_x86.o \
                 kernel/arch/x86/test/test_process.o \
                 kernel/arch/x86/test/test_uaccess.o

KTEST_ARM64_OBJS = kernel/arch/arm64/test/test_arch_arm64.o

ifeq ($(ARCH),arm64)
KTOBJS = $(KTEST_SHARED_OBJS) $(KTEST_ARM64_OBJS)
else
KTOBJS = $(KTEST_SHARED_OBJS) $(KTEST_X86_OBJS)
endif
