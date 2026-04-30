#!/usr/bin/env python3
"""Boot ARM64 virt under QEMU with a socket-backed virtio-net peer and
verify the guest's RX path actually receives a frame the host injects.

Wire-up:
  - this script listens on tcp:127.0.0.1:11000
  - QEMU is launched with `-netdev socket,id=n0,connect=127.0.0.1:11000`
    paired with `-device virtio-net-device,netdev=n0,mac=...`
  - QEMU connects to our listener; once connected we send one frame
    using QEMU's stream-socket framing (4-byte big-endian length
    prefix + raw Ethernet bytes)
  - the in-kernel KTEST `test_arm64_virtio_net_rx_receives_host_frame`
    polls `arm64_virt_virtio_net_rx_packets()` for up to ~2 seconds;
    when it sees a frame, it dequeues and asserts the well-known
    signature, otherwise it skip-passes
  - this script asserts (a) the kernel logged that it detected an
    inbound frame and (b) the SUMMARY reports fail=0

The well-known frame:
  dst = 52:54:00:0d:00:01 (guest mac= advertised in QEMU args)
  src = de:ad:be:ef:00:01 (host harness)
  EtherType = 0x88b5 (IEEE 802 reserved local experimental)
  payload = b"drunix-host-ping" (16 bytes)
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import threading
import time

import arm64_qemu_harness as harness


SUMMARY = "SUMMARY pass="
SUCCESS = "fail=0"
INBOUND_DETECTED = (
    "virtio-net: integration ktest detected inbound frame"
)
TIMEOUT = 30.0
PORT = 11000

WELL_KNOWN_FRAME = (
    bytes.fromhex("525400 0d 0001".replace(" ", ""))
    + bytes.fromhex("dead beef 0001".replace(" ", ""))
    + b"\x88\xb5"
    + b"drunix-host-ping"
)


def ktest_summary_lines(text: str) -> list[str]:
    return [
        line for line in text.splitlines() if "KTEST" in line and SUMMARY in line
    ]


DRIVER_OK_MARKER = "virtio-net: netdev registered (/dev/net0)"


def _send_frame_after_driver_ok(server: socket.socket,
                                serial_log,
                                sent_event: threading.Event,
                                error_holder: list[BaseException]) -> None:
    """Accept QEMU's connection, wait until the guest's virtio-net
    driver is fully ready, then send the well-known frame.

    Sending earlier risks the device dropping the frame because the
    guest hasn't posted RX buffers yet. We watch the serial log for
    the netdev_register marker (which fires after DRIVER_OK and after
    netdev_signal_rx is wired) before injecting.

    QEMU's stream socket netdev expects each frame as 4-byte
    big-endian length prefix + raw Ethernet bytes (qemu/net/socket.c
    net_socket_send).
    """
    try:
        server.settimeout(15.0)
        conn, _ = server.accept()
        try:
            conn.settimeout(5.0)
            # Spin-wait (with a generous deadline) for the guest to
            # log that netdev is registered. Once we see that, the
            # virtio-net device has DRIVER_OK and RX buffers posted.
            deadline = time.time() + 10.0
            while time.time() < deadline:
                try:
                    text = serial_log.read_text()
                except FileNotFoundError:
                    text = ""
                if DRIVER_OK_MARKER in text:
                    break
                time.sleep(0.05)
            length = struct.pack(">I", len(WELL_KNOWN_FRAME))
            conn.sendall(length + WELL_KNOWN_FRAME)
            sent_event.set()
            # Stay around briefly in case QEMU needs the connection
            # alive; close on test completion.
            time.sleep(20.0)
        finally:
            conn.close()
    except BaseException as exc:  # noqa: BLE001 — propagate to caller
        error_holder.append(exc)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()

    build_args = [
        "KTEST=1",
        "ROOT_FS=dufs",
        "PLATFORM=virt",
    ]
    harness.build(build_args)

    serial_log = (
        harness.ROOT / "logs" / "serial-arm64-ktest-virt-net-integration.log"
    )
    stderr_log = (
        harness.ROOT / "logs" / "qemu-arm64-ktest-virt-net-integration.stderr"
    )

    # Listen BEFORE launching QEMU so the QEMU `connect=` retry window
    # finds the listener immediately.
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", PORT))
    server.listen(1)

    sent_event = threading.Event()
    error_holder: list[BaseException] = []
    sender = threading.Thread(
        target=_send_frame_after_driver_ok,
        args=(server, serial_log, sent_event, error_holder),
        daemon=True,
    )
    sender.start()

    netdev = f"socket,id=n0,connect=127.0.0.1:{PORT}"

    def boot_done(text: str) -> bool:
        # Stop polling as soon as the SUMMARY is in the log.
        return bool(ktest_summary_lines(text))

    result = harness.boot_and_wait(
        serial_log,
        stderr_log,
        done=boot_done,
        timeout=TIMEOUT,
        platform="virt",
        netdev=netdev,
    )

    # Close the server (the sender thread will detect this on its
    # close path and exit).
    try:
        server.close()
    except OSError:
        pass

    if error_holder:
        print(f"frame-injector thread raised: {error_holder[0]!r}")
        return 1

    if not sent_event.is_set():
        print("frame injector never accepted a QEMU connection")
        if result.stderr:
            print(result.stderr)
        if result.text:
            print(result.text[-2000:], end="")
        return 1

    summary = ktest_summary_lines(result.text)
    if not summary:
        print("missing ARM64 KTEST summary (virt-net-integration)")
        if result.stderr:
            print(result.stderr)
        if result.text:
            print(result.text[-2000:], end="")
        return 1

    if not any(SUCCESS in line for line in summary):
        print("ARM64 KTEST summary did not report fail=0 "
              "(virt-net-integration)")
        print("\n".join(summary))
        return 1

    if INBOUND_DETECTED not in result.text:
        print(
            "kernel did not log inbound-frame detection "
            "(virt-net-integration); the integration assertion was "
            "skip-passed instead of executed"
        )
        print(result.text[-2000:], end="")
        return 1

    print("arm64 kernel unit tests passed (virt-net-integration)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
