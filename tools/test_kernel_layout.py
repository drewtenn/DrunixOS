import unittest
from pathlib import Path


class KernelLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        cls.objects_mk = (cls.root / "kernel/objects.mk").read_text()
        cls.makefile = (cls.root / "Makefile").read_text()
        cls.arm_arch_mk = (cls.root / "kernel/arch/arm64/arch.mk").read_text()

    def test_x86_specific_objects_live_under_arch_x86(self):
        self.assertIn("kernel/arch/x86/boot/kernel-entry.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/boot/framebuffer_multiboot.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/start_kernel.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/module.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/module_exports.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/proc/core.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/proc/syscall.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/mm/pmm.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/mm/paging.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/mm/paging_asm.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/proc/process_asm.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/proc/switch.o", self.objects_mk)

        self.assertNotIn("kernel/kernel-entry.o", self.objects_mk)
        self.assertNotIn("kernel/kernel.o", self.objects_mk)
        self.assertNotIn("kernel/module.o", self.objects_mk)
        self.assertNotIn("kernel/module_exports.o", self.objects_mk)
        self.assertNotIn("kernel/proc/core.o", self.objects_mk)
        self.assertNotIn("kernel/proc/syscall.o", self.objects_mk)
        self.assertNotIn("kernel/gui/framebuffer_multiboot.o", self.objects_mk)
        self.assertNotIn("kernel/mm/pmm.o", self.objects_mk)
        self.assertNotIn("kernel/mm/paging.o", self.objects_mk)
        self.assertNotIn("kernel/mm/paging_asm.o", self.objects_mk)
        self.assertNotIn("kernel/proc/process_asm.o", self.objects_mk)
        self.assertNotIn("kernel/proc/switch.o", self.objects_mk)

    def test_pc_device_objects_live_under_arch_x86_platform_pc(self):
        self.assertIn("kernel/arch/x86/platform/pc/ata.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/platform/pc/keyboard.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/platform/pc/mouse.o", self.objects_mk)

        self.assertNotIn("kernel/platform/pc/ata.o", self.objects_mk)
        self.assertNotIn("kernel/platform/pc/keyboard.o", self.objects_mk)
        self.assertNotIn("kernel/platform/pc/mouse.o", self.objects_mk)
        self.assertNotIn("kernel/drivers/ata.o", self.objects_mk)
        self.assertNotIn("kernel/drivers/keyboard.o", self.objects_mk)
        self.assertNotIn("kernel/drivers/mouse.o", self.objects_mk)

    def test_makefile_uses_arch_and_platform_include_roots(self):
        self.assertIn("-I kernel/arch/$(ARCH)/mm", self.makefile)
        self.assertIn("-I kernel/arch/$(ARCH)/proc", self.makefile)
        self.assertIn("-I kernel/arch/$(ARCH)/boot", self.makefile)
        self.assertIn("-I kernel/arch/x86/platform/pc", self.makefile)
        self.assertIn("-I kernel/arch/arm64/proc", self.arm_arch_mk)
        self.assertIn("-I kernel/console", self.arm_arch_mk)
        self.assertIn("kernel/arch/x86/linker.ld", self.makefile)
        self.assertIn("kernel/arch/x86/boot/kernel-entry.asm", self.makefile)

        self.assertNotIn("-I kernel/platform/pc", self.makefile)
        self.assertNotIn("-T kernel/kernel.ld", self.makefile)
        self.assertNotIn("kernel/kernel-entry-vga.o: kernel/kernel-entry.asm",
                         self.makefile)

    def test_arm64_shared_lib_objects_use_arch_specific_output_paths(self):
        self.assertIn("kernel/lib/kprintf.arm64.o", self.arm_arch_mk)
        self.assertIn("kernel/lib/kstring.arm64.o", self.arm_arch_mk)
        self.assertNotIn("kernel/lib/kprintf.o \\", self.arm_arch_mk)
        self.assertNotIn("kernel/lib/kstring.o", self.arm_arch_mk)
        self.assertIn("kernel/lib/%.arm64.o: kernel/lib/%.c", self.makefile)

    def test_expected_paths_exist(self):
        expected = [
            "kernel/arch/x86/boot/kernel-entry.asm",
            "kernel/arch/x86/boot/framebuffer_multiboot.c",
            "kernel/arch/x86/boot/framebuffer_multiboot.h",
            "kernel/arch/x86/linker.ld",
            "kernel/arch/x86/module.c",
            "kernel/arch/x86/module_exports.c",
            "kernel/arch/x86/proc/core.c",
            "kernel/arch/x86/proc/syscall.c",
            "kernel/arch/x86/proc/syscall_numbers.h",
            "kernel/arch/x86/mm/pmm.c",
            "kernel/arch/x86/mm/pmm.h",
            "kernel/arch/x86/mm/paging.c",
            "kernel/arch/x86/mm/paging.h",
            "kernel/arch/x86/mm/paging_asm.asm",
            "kernel/arch/x86/proc/process_asm.asm",
            "kernel/arch/x86/proc/switch.asm",
            "kernel/arch/x86/test/test_pmm.c",
            "kernel/arch/x86/test/test_arch_x86.c",
            "kernel/arch/x86/test/test_process.c",
            "kernel/arch/x86/test/test_uaccess.c",
            "kernel/arch/x86/platform/pc/ata.c",
            "kernel/arch/x86/platform/pc/ata.h",
            "kernel/arch/x86/platform/pc/keyboard.c",
            "kernel/arch/x86/platform/pc/keyboard.h",
            "kernel/arch/x86/platform/pc/mouse.c",
            "kernel/arch/x86/platform/pc/mouse.h",
            "kernel/arch/arm64/proc/core.c",
            "kernel/arch/arm64/proc/syscall.c",
            "kernel/arch/arm64/proc/syscall_numbers.h",
            "kernel/arch/arm64/test/test_arch_arm64.c",
        ]

        for relpath in expected:
            with self.subTest(path=relpath):
                self.assertTrue((self.root / relpath).exists(), relpath)


if __name__ == "__main__":
    unittest.main()
