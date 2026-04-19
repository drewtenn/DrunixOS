import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_compile_commands as gccdb


class GenerateCompileCommandsTest(unittest.TestCase):
    def test_kernel_objects_become_compile_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "kernel/proc").mkdir(parents=True)
            (root / "kernel/proc/syscall.c").write_text("int x;\n")

            commands = gccdb.build_commands(
                root=root,
                kernel_objs=["kernel/proc/syscall.o", "kernel/arch/idt_asm.o"],
                kernel_cc="x86_64-elf-gcc",
                kernel_cflags="-m32 -ffreestanding",
                kernel_inc="-I kernel -I kernel/proc",
                user_cc="x86_64-elf-gcc",
                user_cflags="-m32 -nostdlib",
                linux_cc="i486-linux-musl-gcc",
                linux_cflags="-static",
                user_c_runtime_objs=[],
                user_c_progs=[],
                linux_c_progs=[],
            )

            self.assertEqual(len(commands), 1)
            self.assertEqual(commands[0]["file"], str(root / "kernel/proc/syscall.c"))
            self.assertIn("-I kernel/proc", commands[0]["command"])
            self.assertIn("-o kernel/proc/syscall.o", commands[0]["command"])

    def test_user_and_linux_c_programs_use_their_own_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "user/lib").mkdir(parents=True)
            (root / "user/shell.c").write_text("int main(void) { return 0; }\n")
            (root / "user/linuxabi.c").write_text("int main(void) { return 0; }\n")
            (root / "user/lib/stdio.c").write_text("int stdio;\n")

            commands = gccdb.build_commands(
                root=root,
                kernel_objs=[],
                kernel_cc="x86_64-elf-gcc",
                kernel_cflags="-m32 -ffreestanding",
                kernel_inc="-I kernel",
                user_cc="x86_64-elf-gcc",
                user_cflags="-m32 -nostdlib",
                linux_cc="i486-linux-musl-gcc",
                linux_cflags="-static -Os",
                user_c_runtime_objs=["lib/stdio.o", "lib/crt0.o"],
                user_c_progs=["shell"],
                linux_c_progs=["linuxabi"],
            )

            by_file = {Path(entry["file"]).name: entry for entry in commands}
            self.assertIn("x86_64-elf-gcc -m32 -nostdlib", by_file["shell.c"]["command"])
            self.assertIn("-o user/shell.o", by_file["shell.c"]["command"])
            self.assertIn("x86_64-elf-gcc -m32 -nostdlib", by_file["stdio.c"]["command"])
            self.assertIn("-o user/lib/stdio.o", by_file["stdio.c"]["command"])
            self.assertIn("i486-linux-musl-gcc -static -Os", by_file["linuxabi.c"]["command"])

    def test_main_writes_json_database(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / "compile_commands.json"
            (root / "kernel").mkdir()
            (root / "kernel/kernel.c").write_text("int kernel;\n")

            rc = gccdb.main(
                [
                    "--root",
                    str(root),
                    "--output",
                    str(output),
                    "--kernel-objs",
                    "kernel/kernel.o",
                    "--kernel-cc",
                    "cc",
                    "--kernel-cflags=-m32",
                    "--kernel-inc",
                    "-I kernel",
                    "--user-cc",
                    "cc",
                    "--user-cflags=-m32",
                    "--linux-cc",
                    "linux-cc",
                    "--linux-cflags=-static",
                ]
            )

            self.assertEqual(rc, 0)
            data = json.loads(output.read_text())
            self.assertEqual(data[0]["file"], str((root / "kernel/kernel.c").resolve()))


if __name__ == "__main__":
    unittest.main()
