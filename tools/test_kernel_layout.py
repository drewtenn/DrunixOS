import unittest
from pathlib import Path


class KernelLayoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        cls.objects_mk = (cls.root / "kernel/objects.mk").read_text()
        cls.makefile = (cls.root / "Makefile").read_text()

    def test_x86_specific_objects_live_under_arch_x86(self):
        self.assertIn("kernel/arch/x86/boot/kernel-entry.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/mm/pmm.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/mm/paging.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/mm/paging_asm.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/proc/process_asm.o", self.objects_mk)
        self.assertIn("kernel/arch/x86/proc/switch.o", self.objects_mk)

        self.assertNotIn("kernel/kernel-entry.o", self.objects_mk)
        self.assertNotIn("kernel/mm/pmm.o", self.objects_mk)
        self.assertNotIn("kernel/mm/paging.o", self.objects_mk)
        self.assertNotIn("kernel/mm/paging_asm.o", self.objects_mk)
        self.assertNotIn("kernel/proc/process_asm.o", self.objects_mk)
        self.assertNotIn("kernel/proc/switch.o", self.objects_mk)

    def test_pc_device_objects_live_under_platform_pc(self):
        self.assertIn("kernel/platform/pc/ata.o", self.objects_mk)
        self.assertIn("kernel/platform/pc/keyboard.o", self.objects_mk)
        self.assertIn("kernel/platform/pc/mouse.o", self.objects_mk)

        self.assertNotIn("kernel/drivers/ata.o", self.objects_mk)
        self.assertNotIn("kernel/drivers/keyboard.o", self.objects_mk)
        self.assertNotIn("kernel/drivers/mouse.o", self.objects_mk)

    def test_makefile_uses_arch_and_platform_include_roots(self):
        self.assertIn("-I kernel/arch/$(ARCH)/mm", self.makefile)
        self.assertIn("-I kernel/platform/pc", self.makefile)
        self.assertIn("kernel/arch/x86/linker.ld", self.makefile)
        self.assertIn("kernel/arch/x86/boot/kernel-entry.asm", self.makefile)

        self.assertNotIn("-T kernel/kernel.ld", self.makefile)
        self.assertNotIn("kernel/kernel-entry-vga.o: kernel/kernel-entry.asm",
                         self.makefile)

    def test_expected_paths_exist(self):
        expected = [
            "kernel/arch/x86/boot/kernel-entry.asm",
            "kernel/arch/x86/linker.ld",
            "kernel/arch/x86/mm/pmm.c",
            "kernel/arch/x86/mm/pmm.h",
            "kernel/arch/x86/mm/paging.c",
            "kernel/arch/x86/mm/paging.h",
            "kernel/arch/x86/mm/paging_asm.asm",
            "kernel/arch/x86/proc/process_asm.asm",
            "kernel/arch/x86/proc/switch.asm",
            "kernel/platform/pc/ata.c",
            "kernel/platform/pc/ata.h",
            "kernel/platform/pc/keyboard.c",
            "kernel/platform/pc/keyboard.h",
            "kernel/platform/pc/mouse.c",
            "kernel/platform/pc/mouse.h",
        ]

        for relpath in expected:
            with self.subTest(path=relpath):
                self.assertTrue((self.root / relpath).exists(), relpath)


if __name__ == "__main__":
    unittest.main()
