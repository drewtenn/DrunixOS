#!/usr/bin/env python3
"""Check that target-specific kernel sources live under kernel/arch."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

COMMON_START = ROOT / "kernel/kernel.c"
X86_START = ROOT / "kernel/arch/x86/start_kernel.c"
X86_PC_PLATFORM = ROOT / "kernel/arch/x86/platform/pc"
X86_BOOT = ROOT / "kernel/arch/x86/boot"
ARM64_PLATFORM = ROOT / "kernel/arch/arm64/platform"

X86_ONLY_MARKERS = (
    '#include "pmm.h"',
    '#include "paging.h"',
    '#include "ata.h"',
    '#include "gdt.h"',
    '#include "idt.h"',
    '#include "sse.h"',
    '#include "clock.h"',
    '#include "keyboard.h"',
    "#define VIDEO_ADDRESS 0xb8000",
    "start_kernel(uint32_t magic, multiboot_info_t *mbi)",
)

FORBIDDEN_TARGET_ROOTS = (
    ROOT / "kernel/platform/pc",
    ROOT / "kernel/platform/raspi3b",
)

FORBIDDEN_TARGET_FILES = (
    ROOT / "kernel/module.c",
    ROOT / "kernel/module_exports.c",
    ROOT / "kernel/gui/framebuffer_multiboot.c",
    ROOT / "kernel/proc/core.c",
    ROOT / "kernel/proc/syscall.c",
    ROOT / "kernel/test/test_pmm.c",
    ROOT / "kernel/test/test_process.c",
    ROOT / "kernel/test/test_uaccess.c",
    ROOT / "kernel/test/test_desktop.c",
    ROOT / "kernel/test/test_arch_arm64.c",
)

REQUIRED_ARCH_LOCAL_FILES = (
    X86_START,
    ROOT / "kernel/arch/x86/module.c",
    ROOT / "kernel/arch/x86/module_exports.c",
    X86_BOOT / "framebuffer_multiboot.c",
    X86_BOOT / "framebuffer_multiboot.h",
    ROOT / "kernel/arch/x86/proc/core.c",
    ROOT / "kernel/arch/x86/proc/syscall.c",
    ROOT / "kernel/arch/x86/proc/syscall_numbers.h",
    ROOT / "kernel/arch/x86/test/test_pmm.c",
    ROOT / "kernel/arch/x86/test/test_arch_x86.c",
    ROOT / "kernel/arch/x86/test/test_process.c",
    ROOT / "kernel/arch/x86/test/test_uaccess.c",
    ROOT / "kernel/arch/x86/test/test_desktop.c",
    X86_PC_PLATFORM / "ata.c",
    X86_PC_PLATFORM / "keyboard.c",
    X86_PC_PLATFORM / "mouse.c",
    ROOT / "kernel/arch/arm64/proc/core.c",
    ROOT / "kernel/arch/arm64/proc/syscall.c",
    ROOT / "kernel/arch/arm64/proc/syscall_numbers.h",
    ROOT / "kernel/arch/arm64/test/test_arch_arm64.c",
    ARM64_PLATFORM / "platform.h",
    ARM64_PLATFORM / "raspi3b/platform.h",
    ARM64_PLATFORM / "raspi3b/uart.c",
    ARM64_PLATFORM / "raspi3b/irq.c",
    ARM64_PLATFORM / "raspi3b/video.c",
    ARM64_PLATFORM / "raspi3b/usb_hci.c",
    ARM64_PLATFORM / "raspi3b/emmc.c",
)


def main() -> int:
    failures: list[str] = []

    if COMMON_START.exists():
        text = COMMON_START.read_text(encoding="utf-8", errors="ignore")
        for marker in X86_ONLY_MARKERS:
            if marker in text:
                failures.append(f"kernel/kernel.c still contains x86 startup marker {marker!r}")

    for root in FORBIDDEN_TARGET_ROOTS:
        if root.exists():
            leftovers = [
                path.relative_to(ROOT)
                for path in sorted(root.rglob("*"))
                if path.is_file() and path.suffix in {".c", ".h", ".S", ".asm"}
            ]
            for leftover in leftovers:
                failures.append(f"target-specific source remains outside arch: {leftover}")

    for path in FORBIDDEN_TARGET_FILES:
        if path.exists():
            failures.append(f"target-specific source remains outside arch: {path.relative_to(ROOT)}")

    for path in REQUIRED_ARCH_LOCAL_FILES:
        if not path.exists():
            failures.append(f"missing arch-local target source: {path.relative_to(ROOT)}")

    objects = (ROOT / "kernel/objects.mk").read_text()
    if "kernel/kernel.o" in objects:
        failures.append("kernel/objects.mk still builds kernel/kernel.o")
    if "kernel/arch/x86/start_kernel.o" not in objects:
        failures.append("kernel/objects.mk does not build kernel/arch/x86/start_kernel.o")
    for old_obj in (
        "kernel/module.o",
        "kernel/module_exports.o",
        "kernel/gui/framebuffer_multiboot.o",
    ):
        if old_obj in objects:
            failures.append(f"kernel/objects.mk still builds {old_obj}")
    for obj in (
        "kernel/arch/x86/module.o",
        "kernel/arch/x86/module_exports.o",
        "kernel/arch/x86/boot/framebuffer_multiboot.o",
        "kernel/arch/x86/proc/core.o",
        "kernel/arch/x86/proc/syscall.o",
    ):
        if obj not in objects:
            failures.append(f"kernel/objects.mk does not build {obj}")
    for old_obj in ("kernel/proc/core.o", "kernel/proc/syscall.o"):
        if old_obj in objects:
            failures.append(f"kernel/objects.mk still builds {old_obj}")
    if "kernel/platform/pc/" in objects:
        failures.append("kernel/objects.mk still builds PC platform objects outside arch")
    for obj in (
        "kernel/arch/x86/platform/pc/ata.o",
        "kernel/arch/x86/platform/pc/keyboard.o",
        "kernel/arch/x86/platform/pc/mouse.o",
    ):
        if obj not in objects:
            failures.append(f"kernel/objects.mk does not build {obj}")

    makefile = (ROOT / "Makefile").read_text()
    if "kernel/kernel.o:" in makefile:
        failures.append("Makefile still has sentinel rules for kernel/kernel.o")
    if "kernel/arch/x86/start_kernel.o:" not in makefile:
        failures.append("Makefile does not have sentinel rules for kernel/arch/x86/start_kernel.o")
    if "kernel/platform/pc/" in makefile:
        failures.append("Makefile still references PC platform paths outside arch")

    framebuffer_header = (ROOT / "kernel/gui/framebuffer.h").read_text()
    if "multiboot_info" in framebuffer_header:
        failures.append("kernel/gui/framebuffer.h still exposes Multiboot-specific API")

    syscall_header = (ROOT / "kernel/proc/syscall.h").read_text()
    if "#define SYS_EXIT " in syscall_header:
        failures.append("kernel/proc/syscall.h still owns target syscall numbers")

    arm_arch_mk = (ROOT / "kernel/arch/arm64/arch.mk").read_text()
    if "kernel/platform/raspi3b/" in arm_arch_mk:
        failures.append("arm64 arch.mk still builds Raspberry Pi platform objects outside arch")
    for obj in (
        "kernel/arch/arm64/platform/raspi3b/uart.o",
        "kernel/arch/arm64/platform/raspi3b/irq.o",
        "kernel/arch/arm64/platform/raspi3b/video.o",
        "kernel/arch/arm64/platform/raspi3b/usb_hci.o",
        "kernel/arch/arm64/platform/raspi3b/emmc.o",
    ):
        if obj not in arm_arch_mk:
            failures.append(f"arm64 arch.mk does not build {obj}")
    for obj in (
        "kernel/arch/arm64/proc/core.arm64.o",
        "kernel/arch/arm64/proc/syscall.arm64.o",
    ):
        if obj not in arm_arch_mk:
            failures.append(f"arm64 arch.mk does not build {obj}")
    for old_obj in ("kernel/proc/core.arm64.o", "kernel/proc/syscall.arm64.o"):
        if old_obj in arm_arch_mk:
            failures.append(f"arm64 arch.mk still builds {old_obj}")

    if failures:
        print("target-specific source architecture boundary check failed:")
        for failure in failures:
            print(f"  {failure}")
        return 1

    print("target-specific source architecture boundary check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
