"""arm64_qemu_harness — shared scaffolding for tools/test_arm64_*.py.

Each ARM64 test boots the same kernel image in QEMU with the same flags
(machine raspi3b, headless, primary serial null, secondary serial → log
file, no monitor, no reboot) and watches the serial log for marker
strings.  Centralising that here keeps the per-test scripts tiny and
ensures one place to update if the QEMU invocation changes.

The simple wrapper `run_arm64_test()` covers the majority case:
build → boot → wait for markers → assert → print.  Tests that need
multiple boots, the QMP socket, or post-test cleanup use the lower
level helpers (build, qemu_command, boot_and_wait).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence
import os
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


def qemu_binary() -> str:
    return os.environ.get("QEMU_ARM", "qemu-system-aarch64")


def qemu_machine() -> str:
    return os.environ.get("QEMU_ARM_MACHINE", "raspi3b")


def build(make_args: Sequence[str] = ()) -> None:
    """Run `make ARCH=arm64 build` with optional extra args.  Raises on failure."""
    result = subprocess.run(
        ["make", "ARCH=arm64", "build", *make_args],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            "arm64 build failed:\n"
            + (result.stderr.strip() or result.stdout.strip() or "no build output")
        )


def refresh_boot_image() -> None:
    """Force a relink + objcopy by removing the cached boot artefacts."""
    for path in (
        ROOT / "kernel" / "arch" / "arm64" / "start_kernel.o",
        ROOT / "kernel-arm64.elf",
        ROOT / "kernel8.img",
    ):
        path.unlink(missing_ok=True)


def qemu_command(serial_log: Path) -> list[str]:
    """The canonical headless QEMU command-line for ARM64 boots."""
    return [
        qemu_binary(),
        "-display", "none",
        "-M", qemu_machine(),
        "-kernel", "kernel-arm64.elf",
        "-drive", "if=sd,format=raw,file=img/disk.img",
        "-serial", "null",
        "-serial", f"file:{serial_log}",
        "-monitor", "none",
        "-no-reboot",
    ]


@dataclass
class BootResult:
    text: str        # contents of the serial log at exit
    stderr: str      # captured QEMU stderr
    returncode: int | None  # QEMU return code, or None if killed
    timed_out: bool


def boot_and_wait(serial_log: Path,
                  stderr_log: Path,
                  done: Callable[[str], bool],
                  timeout: float = 20.0) -> BootResult:
    """Boot the kernel under QEMU; poll the serial log until `done(text)`.

    `done` is invoked with the current contents of the serial log on
    each tick; return True to stop the wait.  When `done` is not
    satisfied within `timeout` seconds (or QEMU exits first), the
    function still returns — callers decide what counts as failure.
    """
    serial_log.parent.mkdir(exist_ok=True)
    serial_log.unlink(missing_ok=True)
    stderr_log.unlink(missing_ok=True)

    timed_out = False
    rc: int | None = None
    with stderr_log.open("w") as stderr:
        try:
            proc = subprocess.Popen(
                qemu_command(serial_log),
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=stderr,
            )
        except FileNotFoundError as exc:
            raise SystemExit(
                f"failed to start QEMU command {qemu_binary()!r}: {exc}"
            ) from exc

        try:
            deadline = time.time() + timeout
            while time.time() < deadline:
                text = _read(serial_log)
                if done(text):
                    break
                rc = proc.poll()
                if rc is not None:
                    break
                time.sleep(0.5)
            else:
                timed_out = True
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            rc = proc.returncode

    return BootResult(
        text=_read(serial_log),
        stderr=stderr_log.read_text(errors="ignore").strip(),
        returncode=rc,
        timed_out=timed_out,
    )


def all_markers_seen(required: Iterable[str]) -> Callable[[str], bool]:
    """Convenience predicate: every required string is present."""
    required = list(required)
    return lambda text: all(m in text for m in required)


def assert_markers(result: BootResult,
                   required: Iterable[str],
                   forbidden: Iterable[str] = ()) -> None:
    """Raise SystemExit if any required marker is missing or any forbidden marker present."""
    missing = [m for m in required if m not in result.text]
    if missing:
        details = f"\nstderr:\n{result.stderr}" if result.stderr else "\nstderr: <empty>"
        raise SystemExit("missing markers: " + ", ".join(missing) + details)

    seen_forbidden = [m for m in forbidden if m in result.text]
    if seen_forbidden:
        raise SystemExit("forbidden markers present: " + ", ".join(seen_forbidden))


def run_arm64_test(*,
                   serial_log_name: str,
                   required: Sequence[str],
                   success_message: str,
                   forbidden: Sequence[str] = (),
                   build_args: Sequence[str] = (),
                   timeout: float = 20.0,
                   refresh: bool = False) -> int:
    """All-in-one wrapper for the common build/boot/check pattern."""
    if refresh:
        refresh_boot_image()
    build(build_args)

    serial_log = ROOT / "logs" / serial_log_name
    stderr_log = serial_log.with_suffix(".stderr")
    result = boot_and_wait(
        serial_log,
        stderr_log,
        all_markers_seen(required),
        timeout=timeout,
    )
    assert_markers(result, required, forbidden)
    print(success_message)
    return 0


def _read(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(errors="ignore")
