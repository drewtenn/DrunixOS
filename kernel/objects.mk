KOBJS = kernel/arch/x86/boot/kernel-entry.o kernel/arch/x86/start_kernel.o \
        kernel/proc/init_launch.o \
        kernel/arch/x86/module.o kernel/arch/x86/module_exports.o \
        kernel/lib/klog.o \
        kernel/lib/kstring.o kernel/lib/kprintf.o kernel/lib/ksort.o \
        kernel/console/terminal.o kernel/console/runtime.o \
        kernel/arch/x86/arch.o \
        kernel/arch/x86/gdt.o kernel/arch/x86/gdt_flush.o \
        kernel/arch/x86/idt.o kernel/arch/x86/isr.o kernel/arch/x86/sse.o kernel/arch/x86/df_test.o \
        kernel/arch/x86/irq.o kernel/arch/x86/pit.o kernel/arch/x86/clock.o \
        kernel/arch/x86/platform/pc/keyboard.o kernel/arch/x86/platform/pc/mouse.o kernel/arch/x86/platform/pc/ata.o \
        kernel/drivers/blkdev.o kernel/drivers/blkdev_part.o kernel/blk/bcache.o kernel/drivers/chardev.o kernel/drivers/tty.o kernel/drivers/fbdev.o kernel/drivers/inputdev.o kernel/drivers/wmdev.o shared/kbdmap.o \
        kernel/gui/display.o kernel/gui/framebuffer.o kernel/arch/x86/boot/framebuffer_multiboot.o kernel/gui/font8x16.o \
        kernel/mm/pmm_core.o kernel/arch/x86/mm/pmm.o kernel/arch/x86/mm/paging.o kernel/arch/x86/mm/paging_asm.o kernel/mm/fault.o kernel/mm/vma.o kernel/mm/kheap.o kernel/mm/slab.o \
        kernel/proc/elf.o kernel/proc/process.o kernel/arch/x86/proc/arch_proc.o kernel/arch/x86/proc/process_asm.o kernel/proc/task_group.o kernel/proc/resources.o \
        kernel/proc/sched.o kernel/arch/x86/proc/syscall.o kernel/proc/syscall/helpers.o kernel/proc/syscall/fd.o kernel/proc/syscall/fd_control.o kernel/proc/syscall/vfs/open.o kernel/proc/syscall/vfs/path.o kernel/proc/syscall/vfs/stat.o kernel/proc/syscall/vfs/dirents.o kernel/proc/syscall/vfs/mutation.o kernel/proc/syscall/task.o kernel/proc/syscall/time.o kernel/proc/syscall/process.o kernel/proc/syscall/info.o kernel/proc/syscall/console.o kernel/proc/syscall/signal.o kernel/proc/syscall/mem.o kernel/proc/syscall/tty.o kernel/arch/x86/proc/core.o kernel/proc/mem_forensics.o kernel/proc/pipe.o kernel/proc/pty.o kernel/arch/x86/proc/switch.o \
        kernel/proc/uaccess.o \
        kernel/fs/fs.o kernel/fs/vfs/core.o kernel/fs/vfs/lookup.o kernel/fs/vfs/mutation.o kernel/fs/procfs.o kernel/fs/sysfs.o kernel/fs/ext3/main.o kernel/fs/ext3/blocks.o kernel/fs/ext3/lookup.o kernel/fs/ext3/mutation.o kernel/fs/ext3/journal.o

KOBJS_VGA = kernel/arch/x86/boot/kernel-entry-vga.o $(filter-out kernel/arch/x86/boot/kernel-entry.o,$(KOBJS))
