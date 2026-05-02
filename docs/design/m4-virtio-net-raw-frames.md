# M4 Design — virtio-net + `/dev/net0` raw Ethernet frames on QEMU `virt`

**Status:** **IMPLEMENTED 2026-05-02.** virt 204/204, virt-net-integration 204/204, virt-ramfb-fallback 184/184, raspi3b 151/151.
**Commits:** `58a7e0f` (1), `6b81156` (2), `b1ac2c2` (3), `d16aaa8` (4), `bbbf872` (4 follow-up), `d7f3b5a` (5), `acb6d08` (6), `9d85813` (7), `752fdc2` (8), this one (9).
**Scope:** First DrunixOS networking subsystem — raw Ethernet frame I/O via virtio-net on QEMU `virt`.
**Plan reference:** `docs/superpowers/plans/2026-04-30-drunix-m4-virtio-net-raw-frames.md`.
**Process:** built from two `/octo:embrace` runs (codex + opencode + claude). The Define→Develop debate gate on the first run pivoted M4 from GPU-MVP Phase 3 to networking; the gate on the second run revised commit 2's scope from v1+v2 to v1-only.

## Implementation outcome

| Commit | Hash | Files | Test deltas |
|---|---|---|---|
| 1 — mmio probe | `58a7e0f` | new `virtio_net.{c,h}`, `arch.mk`, `start_kernel.c`, `test_arch_arm64.c`, `arm64_qemu_harness.py` | virt 184→185 (+1) |
| 2 — feature + MAC | `6b81156` | `virtio_net.{c,h}`, `test_arch_arm64.c`, `arm64_qemu_harness.py` | virt 185→190 (+5) |
| 3 — DMA rings + buffers | `b1ac2c2` | `virtio_net.{c,h}`, `test_arch_arm64.c`, `dma.c` (pool 24→64 pages) | virt 190→193 (+3) |
| 4 — RX refill + IRQ | `d16aaa8` | `virtio_net.{c,h}`, `test_arch_arm64.c` | virt 193→196 (+3) |
| 4 follow-up — ack-then-drain race | `bbbf872` | `virtio_net.{c,h}` | None (tightens IRQ handler) |
| 5 — TX submission | `d7f3b5a` | `virtio_net.{c,h}`, `test_arch_arm64.c` | virt 196→199 (+3) |
| 6 — `/dev/net0` chardev | `acb6d08` | new `netdev.{c,h}`, `chardev.h`, `fd.c`, `objects.mk`, `arch.mk`, `start_kernel.c`, `virtio_net.c`, `test_arch_arm64.c` | virt 199→203 (+4) |
| 7 — user smoke tools | `9d85813` | new `net0send.c`, `net0recv.c`, `programs.mk` | None (user binaries) |
| 8 — integration KTEST | `752fdc2` | `test_arch_arm64.c`, `arm64_qemu_harness.py`, new `test_arm64_virtio_net_integration.py`, `checks.mk` | virt 203→204 (+1); new harness 204/204 |
| 9 — design doc | (this commit) | this file, `docs/contributing/aarch64-dma.md` | None |

## Why

DrunixOS had no networking subsystem. The original plan for the next milestone was GPU-MVP Phase 3 (host codec sidecar prototypes), but the Define→Develop debate gate carried three points: (1) Phase 3 proves macOS codec correctness, not Drunix's ability to expose a durable device contract; (2) two PRD adversarial review passes before any shipped capability is a smell; (3) networking exercises Drunix as an OS, not adjacent to it. Owner accepted the pivot.

Networking unlocks downstream work that codec cannot: package fetches, remote debug, CI test injection, and a foundation for future distributed DriverVM work.

## What's already there (post-arm64-Phase 1, on `main`)

- `virtio_mmio_find()`, `virtio_mmio_enumerate()` — slot scanning and device-id matching.
- `virtq_init`, `virtq_alloc_chain`, `virtq_submit`, `virtq_drain_one`, `virtq_free_chain` — split-ring (legacy) virtqueue mechanics with `arm64_dma_wmb` / `arm64_dma_cache_clean` discipline already wired.
- `virt_dma_alloc` — page-aligned DMA pool (pre-M4: 24 pages = 96 KiB, M4 raised to 64 pages = 256 KiB).
- `virt_irq_register_spi`, `virt_irq_enable_spi` — GICv3 SPI dispatch.
- `kernel/drivers/chardev.{c,h}` — chardev registry with `read`, `read_char`, `write_char`, `mmap_phys`, `ioctl` ops; M4 added a `write` op.
- Existing virtio drivers as worked examples: `virtio_blk.c`, `virtio_input.c`, `virtio_gpu.c`.

## Locked design choices

### 1. Legacy transport (MMIO version 1) only

The probe in commit 1 accepts version 1 or 2; commit 2 explicitly rejects `version != 1` because the legacy register layout (`QUEUE_PFN`) and the modern layout (`QUEUE_DESC_LOW/HIGH`, `QUEUE_DRIVER_LOW/HIGH`, `QUEUE_DEVICE_LOW/HIGH`) are not interchangeable. QEMU's `-M virt` defaults to legacy, so M4 stays there. Modern transport is a documented follow-on.

### 2. Feature subset: `VIRTIO_NET_F_MAC` only

Negotiates F_MAC (page 0 bit 5). All other feature bits are explicitly NOT offered: `MRG_RXBUF`, `CSUM`, all `GUEST_*`/`HOST_*` offload bits, `CTRL_VQ`, `STATUS`. This locks the on-wire `virtio_net_hdr` to the 10-byte legacy form and keeps the device's behavior simple. Link-status visibility (`F_STATUS`, page 1 bit 16) is deferred — adding the SEL=1 page selection plumbing would inflate commit 2 from a handshake into a transport layer.

### 3. Two virtqueues: receiveq (0) and transmitq (1)

VIRTQ_SIZE = 16 for both. `QUEUE_NUM_MAX` must be ≥ 16; both queues are configured with explicit `QUEUE_SEL` writes to avoid implicit-state drift. No control vq (no `CTRL_VQ`).

### 4. DMA-pool-only buffers

All buffers — virtqueue backings, RX packet pool, TX packet pool — come from `virt_dma_alloc`. `virtio_net_slice_pool()` asserts that every per-buffer `virt_virt_to_phys()` translation is non-zero, which catches a regression to stack/static/`kheap` memory before it silently DMAs to physical address 0.

Pool sizes per direction: 16 buffers × 2048 bytes = 32 KiB = 8 pages. Plus 2-page virtqueue backing per direction. Total M4 footprint: 20 pages of the DMA pool. The pool was raised from 24 to 64 pages in commit 3.

### 5. 1:1 descriptor ↔ buffer mapping

Descriptor index `i` always pairs with buffer slot `i`. Both rings use this convention. `virtq_alloc_chain` returns descriptor 0 first on a fresh queue, then 1, etc., so this mapping holds as long as the driver preserves it across alloc/free cycles. `virtq_free_chain` returns descriptors to the free list and implicitly frees the paired buffer slot — no separate buffer free list.

### 6. RX cache discipline as a structural type

The Discover-phase audit identified the worst risk as "invalidate before any header byte read" being policy-by-comment. The Define-revision turned this into a structural enforcement:

```c
typedef struct {
    const uint8_t *frame;
    uint32_t len;
} virtio_net_rx_token_t;            /* file-scope; no public constructor */

static int virtio_net_rx_invalidate_and_take(uint16_t buf_index,
                                             uint32_t used_len,
                                             virtio_net_rx_token_t *out);

static void virtio_net_publish_frame(const virtio_net_rx_token_t *token);
```

`virtio_net_rx_invalidate_and_take()` is the only producer of the token; it performs `arm64_dma_cache_invalidate()` and validates the length bounds before constructing the token. `virtio_net_publish_frame()` consumes by const pointer to a stack token. A bypass that reads the buffer directly would have to fabricate a token, which is at least visible in code review.

### 7. IRQ handler with two-queue drain + ack-then-drain race fix

The handler acks both `INTERRUPT_STATUS` bit 0 (used-ring) and bit 1 (config-change) by writing the snapshot back; config-change is acked-and-ignored until link-status tracking arrives. After acking, the handler drains both queues:

- RX: `virtq_drain_one` → `invalidate_and_take` → `publish_frame` (or drop) → `refill_one`
- TX: `virtq_drain_one` → `virtq_free_chain`

An outer guard loop re-polls both queues after the inner drain returns empty. Without this, a completion published between drain-empty and handler-return would not raise a fresh interrupt edge (we already acked) and would stay stuck until something else nudged the device. The outer is bounded by `VIRTQ_SIZE+1` iterations as a paranoia stop. This race was caught by code review on commit 4 and fixed in `bbbf872`.

### 8. Driver-local RX ring (SPSC)

8-slot ring inside `virtio_net.c` holds Ethernet frames pulled off the virtqueue's used ring by the IRQ handler, awaiting pickup by the consumer. The IRQ handler is the producer; `arm64_virt_virtio_net_rx_dequeue()` is the consumer. Drops increment `rx_drops_ring_full`.

### 9. Single-producer TX

`arm64_virt_virtio_net_send_frame()` is single-producer for M4. The only producer in v1 is the `/dev/net0` chardev's `write()` path, which is single-threaded. SMP makes this a comment to revisit. Validates `14 ≤ len ≤ 1514`; out-of-range → `-1` + `tx_drops_busy++`. Drains pending TX completions before allocating a descriptor, so a transient burst can recover by reaping in-flight buffers.

### 10. `/dev/net0` chardev shim via shared `kernel/drivers/netdev.{c,h}`

Analogous to inputdev. The transport (arm64 virtio-net) registers a `netdev_ops_t = { recv_frame, send_frame }` via `netdev_register()`. The chardev's `read` blocks on a wait queue until `netdev_signal_rx()` fires (called from `virtio_net_publish_frame`); `write` calls through to the registered `send_frame`. The `chardev_ops_t` struct gained a multi-byte `write` op for this; `syscall_write_fd` got a `FD_TYPE_CHARDEV` case that dispatches through it.

Each `read(fd, buf, count)` returns exactly one Ethernet frame (no `virtio_net_hdr` — the transport strips it). Each `write(fd, buf, count)` submits exactly one frame.

## Implementation sequence (revised after delivery review)

### Commit 1 — `arm64/virt: identify virtio-net mmio devices`
Read-only probe of the 32 virtio-mmio slots for DeviceID 1. No MMIO writes. KTEST asserts the recorded MMIO base + slot + version are sane.

### Commit 2 — `arm64/virt: virtio-net feature negotiation and mac discovery`
Reset → ACK → DRIVER → SEL=0 → DRIVER_FEATURES=(1<<F_MAC) → FEATURES_OK readback. Refuses devices that don't advertise F_MAC and devices reporting MMIO version != 1. Reads MAC as 6 byte loads. KTEST asserts FEATURES_OK + non-zero MAC; harness pins `mac=52:54:00:0d:00:01` so a byte-order regression test has a deterministic value.

### Commit 3 — `arm64/virt: allocate virtio-net dma rings and packet buffers`
Four `virt_dma_alloc` allocations (RX/TX virtq backing + RX/TX packet pools), all unwound on partial failure via a single `fail:` label. Configures both queues at the legacy MMIO level. DRIVER_OK is NOT yet set. The DMA pool was raised from 24 to 64 pages here. KTEST asserts every per-buffer phys translation is non-zero.

### Commit 4 — `arm64/virt: virtio-net rx refill and irq completion`
Pre-prime the receiveq with all 16 writable buffers (each invalidated before hand-off so a stale dirty cache line cannot evict over a future device write). Register the GICv3 SPI handler. Set DRIVER_OK and notify queue 0. The IRQ handler implements the structural-token cache discipline.

### Commit 4 follow-up — IRQ ack-then-drain race fix
Outer guard loop re-polls used ring after the inner drain returns empty. Closes the race window between drain-empty and handler return.

### Commit 5 — `arm64/virt: virtio-net tx submission and completion`
Single-producer `arm64_virt_virtio_net_send_frame()` validates length, allocates descriptor + buffer (1:1), copies and cleans, submits, kicks. IRQ handler's outer guard loop now drains TX completions in addition to RX, so a TX completion landing between drain-empty and return is also picked up.

### Commit 6 — `kernel/drivers: add net0 raw frame chardev`
Adds the shared netdev framework. Extends `chardev_ops_t` with a multi-byte `write` op and `syscall_write_fd` with a `FD_TYPE_CHARDEV` dispatch case. virtio-net registers itself via `netdev_register` and signals readers via `netdev_signal_rx`.

### Commit 7 — `user/net: add raw net0 send and recv smoke tools`
`/bin/net0send` writes one well-known frame; `/bin/net0recv` blocks on read and prints the parsed frame. Wired into PROGS and C_PROGS so the disk image carries them.

### Commit 8 — `ktest/arm64/virt: virtio-net integration test`
Socket-backed QEMU netdev (`socket,id=n0,connect=127.0.0.1:11000`) paired with a Python harness that listens, waits for the kernel's `netdev registered` marker, then injects the well-known frame via QEMU's stream-socket framing (4-byte big-endian length prefix). The kernel KTEST polls `rx_packets()` under a wall-clock deadline, dequeues, and asserts the signature. Skip-passes under harnesses that don't inject traffic; runs as part of `make check`.

### Commit 9 — Documentation refresh (this commit)
This file plus a worked-example pass on `docs/contributing/aarch64-dma.md`.

## Integration harness — wire format

The integration test uses QEMU's stream-socket netdev. From `qemu/net/socket.c` `net_socket_send()`:

```
length = htonl(size);                /* 4-byte big-endian length */
iov_send(fd, [length, frame_bytes]); /* one TCP write */
```

Each frame on the wire is 4 bytes of big-endian length followed by the raw Ethernet bytes. The Python harness does the same in reverse to inject inbound frames.

Connection ordering:
1. Python harness opens TCP listener on 127.0.0.1:11000.
2. QEMU launches with `connect=127.0.0.1:11000` and connects to the listener.
3. Python harness's `accept()` thread waits for the kernel to log `virtio-net: netdev registered (/dev/net0)` (which fires after DRIVER_OK and after the netdev-signal hook is wired). Sending earlier risks the device dropping the frame because RX buffers aren't yet posted.
4. Python sends `[BE4-length][frame]` once.
5. QEMU's virtio-net delivers the frame to the guest's RX queue; IRQ fires; `virtio_net_publish_frame` pushes to the driver-local RX ring; `netdev_signal_rx` wakes any blocking reader.
6. Kernel KTEST polls `rx_packets()` under a 2-second wall-clock deadline (matches the virtio-blk poll pattern); on detection it dequeues and asserts the well-known signature.

## Acceptance criteria (all met)

1. `make ARCH=arm64 PLATFORM=virt run` (or `run-headless`) boots cleanly and reaches `virtio-net: netdev registered (/dev/net0)`.
2. `chardev_get("net0")` returns a non-NULL ops table with both `read` and `write` populated.
3. `arm64_virt_virtio_net_send_frame(buf, len)` returns `len` for valid frames, `-1` for length-out-of-range or busy.
4. The Python integration harness can inject a frame that the kernel observes byte-for-byte.
5. `make check` passes the four arm64 KTEST harnesses (`virt`, `raspi3b`, `virt-ramfb-fallback`, `virt-net-integration`).
6. No regressions in any pre-M4 KTEST.

## Out of scope for M4

- IPv4 / IPv6 / IP stack of any kind.
- UDP / TCP / DHCP / DNS / sockets / ARP-in-kernel / routing.
- `curl`, HTTP fetch, TLS, resolver, any application protocol.
- x86_64 virtio-net parity.
- LinuxKPI / Linux source-shim.
- Multi-device support (one virtio-net device only).
- Performance tuning beyond correctness counters and basic diagnostics.
- DriverVM / hypervisor work (v3.1 territory).

## Risks and mitigations

| # | Risk | Status |
|---|---|---|
| R-001 | RX cache invalidate ordering corrupts first real traffic | Mitigated structurally via opaque `virtio_net_rx_token_t` (commit 4). |
| R-002 | Silent DMA-to-zero from non-pool pointers | Mitigated by `virtio_net_slice_pool()` non-zero phys assertion (commit 3). |
| R-003 | IRQ ack-then-drain race | Mitigated by outer guard loop in IRQ handler (commit 4 follow-up). |
| R-004 | Modern transport (v2) silently breaks the legacy handshake | Explicit `version != 1` rejection in commit 2; documented as follow-on. |
| R-005 | Single-producer TX assumption breaks under SMP | Documented in `send_frame` doc comment; revisit with SMP. |
| R-006 | DMA pool exhaustion as more devices arrive | Mitigated by 24→64 pages bump (commit 3); next bump when something new claims more than 30 KiB. |
| R-007 | Integration harness flake | Wait-for-`netdev_registered` ordering before injection; bounded 2-second poll deadline; SO_REUSEADDR on the listener. |

## References

- Plan: `docs/superpowers/plans/2026-04-30-drunix-m4-virtio-net-raw-frames.md`
- DMA discipline: `docs/contributing/aarch64-dma.md`
- C style: `docs/contributing/c-style.md`
- Commit message constraints: `docs/contributing/commits.md`
- Virtio 1.x §5.1 (network device); §2.4 (split-ring layout); §2.6 (memory ordering).
- Reference drivers: `kernel/arch/arm64/platform/virt/virtio_input.c`, `virtio_blk.c`, `virtio_queue.c`.
- QEMU source: `net/socket.c` (stream socket framing).
