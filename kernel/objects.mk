KOBJS = kernel/kernel-entry.o kernel/kernel.o \
        kernel/module.o kernel/module_exports.o \
        kernel/lib/klog.o \
        kernel/lib/kstring.o kernel/lib/kprintf.o kernel/lib/ksort.o \
        kernel/arch/x86/gdt.o kernel/arch/x86/gdt_flush.o \
        kernel/arch/x86/idt.o kernel/arch/x86/isr.o kernel/arch/x86/sse.o kernel/arch/x86/df_test.o \
        kernel/arch/x86/irq.o kernel/arch/x86/pit.o kernel/arch/x86/clock.o \
        kernel/drivers/keyboard.o kernel/drivers/mouse.o kernel/drivers/ata.o \
        kernel/drivers/blkdev.o kernel/drivers/blkdev_part.o kernel/blk/bcache.o kernel/drivers/chardev.o kernel/drivers/tty.o \
        kernel/gui/display.o kernel/gui/framebuffer.o kernel/gui/font8x16.o kernel/gui/desktop.o kernel/gui/desktop_apps.o kernel/gui/terminal.o \
        kernel/mm/pmm.o kernel/mm/paging.o kernel/mm/paging_asm.o kernel/mm/fault.o kernel/mm/vma.o kernel/mm/kheap.o kernel/mm/slab.o \
        kernel/proc/elf.o kernel/proc/process.o kernel/proc/process_asm.o kernel/proc/task_group.o kernel/proc/resources.o \
        kernel/proc/sched.o kernel/proc/syscall.o kernel/proc/syscall/helpers.o kernel/proc/syscall/fd.o kernel/proc/syscall/fd_control.o kernel/proc/syscall/vfs/open.o kernel/proc/syscall/vfs/path.o kernel/proc/syscall/vfs/stat.o kernel/proc/syscall/vfs/dirents.o kernel/proc/syscall/vfs/mutation.o kernel/proc/syscall/task.o kernel/proc/syscall/time.o kernel/proc/syscall/process.o kernel/proc/syscall/info.o kernel/proc/syscall/console.o kernel/proc/syscall/signal.o kernel/proc/syscall/mem.o kernel/proc/syscall/tty.o kernel/proc/core.o kernel/proc/mem_forensics.o kernel/proc/pipe.o kernel/proc/switch.o \
        kernel/proc/uaccess.o \
        kernel/fs/fs.o kernel/fs/vfs/core.o kernel/fs/vfs/lookup.o kernel/fs/vfs/mutation.o kernel/fs/procfs.o kernel/fs/sysfs.o kernel/fs/ext3/main.o kernel/fs/ext3/blocks.o kernel/fs/ext3/lookup.o kernel/fs/ext3/mutation.o kernel/fs/ext3/journal.o

KOBJS_VGA = kernel/kernel-entry-vga.o $(filter-out kernel/kernel-entry.o,$(KOBJS))
