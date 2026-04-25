# ARM64 Working Shell And Native User Programs Plan

## Goal

Make `make ARCH=arm64 all` follow the same default workflow as x86: build the
kernel, build a root filesystem, boot into the framebuffer console, and launch
`/bin/shell` as init. The only user-facing build selector should be `ARCH`.

The default userland is the native Drunix program set from `C_PROGS` and
`CXX_PROGS`. Generated Linux/i386 compatibility payloads (`busybox`, `tcc`,
`readelf`, `objdump`, `nano`, GCC helper payloads, and Linux ABI probes) are
deprecated and removed from the default build/test path.

## Phases

1. Align default build targets.
   - ARM64 `build`, `run`, `run-fresh`, and `all` should mirror the x86 names.
   - ARM64 should default to `INIT_PROGRAM=bin/shell`, `INIT_ARG0=shell`.
   - x86 and ARM64 default disk images should package the same native Drunix
     program manifest.

2. Bring up ARM64 framebuffer shell startup.
   - Keep the ARM64 mailbox/framebuffer path enabled by default.
   - Start the shared shell instead of the old `arm64init` smoke program.
   - Keep `arm64init` available only as a low-level diagnostic binary.

3. Add ARM64 serial/TTY input.
   - Feed ARM64 UART input into `tty0`.
   - Poll input from scheduler idle/tick/read paths so the shell can receive
     commands through QEMU serial stdio.

4. Make ARM64 user processes isolated enough for shell workflows.
   - Give each ARM64 process private user backing pages.
   - Sync the active identity execution window to/from the scheduled process.
   - Deep-copy user pages on fork so `fork` plus `exec` does not overwrite the
     parent shell.

5. Build native ARM64 user programs.
   - Compile C and C++ programs into `build/arm64-user`.
   - Use an AArch64 runtime entry that passes `argc`, `argv`, and `envp`, runs
     constructors/destructors, and exits through the shared syscall wrapper.
   - Use compiler builtin `va_list` support so varargs work on both x86 and
     AArch64.

6. Make the framebuffer console readable.
   - Strip ANSI CSI color sequences in the simple ARM64 framebuffer text
     console so the shell prompt renders as `drunix:/>` instead of raw escape
     bytes.
   - Strengthen the ARM64 VGA smoke to OCR the framebuffer screenshot and
     assert that the shell prompt is visible without leaked ANSI markers.

7. Add true QEMU-window keyboard support.
   - Launch ARM64 QEMU with an explicit keyboard device, likely `-device
     usb-kbd` on the `raspi3b` machine.
   - Implement the minimal BCM2835 DWC2 USB host path needed to enumerate and
     poll a USB HID boot keyboard.
   - Translate HID usages into the same characters and escape sequences the
     x86 keyboard path feeds into `tty_input_char(0, ...)`.

8. Keep tests architecture-neutral where behavior is shared.
   - Use one shell-session harness with architecture-specific QEMU adapters.
   - Use shared shell prompt and user-program smoke tests.
   - Keep ARM64-only tests only for ARM64-only hardware paths such as mailbox
     framebuffer validation.

## Verification

Required checks:

```sh
python3 tools/check_userland_runtime_lanes.py
python3 tools/test_kernel_arch_boundary_phase7.py
make ARCH=x86 build
make ARCH=arm64 check
python3 tools/test_arm64_vga_console.py
```
