# AArch64 DMA Discipline

This file is the rule set for any kernel driver that exchanges in-memory
descriptors or buffers with hardware on the AArch64 ports (today: the
QEMU `virt` platform; tomorrow: bare metal). It closes FR-013 of
`docs/superpowers/specs/2026-04-29-gpu-h264-mvp.md`.

## Scope

Applies to:

- virtio drivers on the QEMU `virt` platform.
- Any future driver on AArch64 that publishes guest memory to hardware
  through descriptor rings, MMIO, or shared memory.
- The shared barrier and cache-maintenance helpers in
  `kernel/arch/arm64/dma.h`.
- The DMA-safe page allocator in
  `kernel/arch/arm64/platform/virt/dma.h` (and any future
  per-platform equivalent).

Does not apply to:

- Userspace memory management. User pages reach virtio via syscalls
  that copy into kernel-owned DMA buffers; the kernel owns the
  cache/barrier policy on those buffers.
- The x86 ports. Those follow the existing `kernel/arch/x86` rules.

## Memory model regimes

Drunix's AArch64 build moves through two memory-model regimes; the
helpers in `dma.h` are written so call sites do not change between
them.

### M2.x: MMU off

`make ARCH=arm64 PLATFORM=virt` runs with the MMU off through Phase 1
M2.3. The CPU treats RAM as Device-nGnRnE: strongly ordered, no
caching, no speculation, no write-merging. Three consequences:

1. Producer and consumer ordering is enforced with DMB barriers
   alone. No `dc cvac` / `dc ivac` is needed because the data caches
   are bypassed for these accesses.
2. Hardware sees stores in program order once a DMB retires; the
   guest does not need to flush anything.
3. All allocators that hand out RAM are technically DMA-safe,
   *because the cache attributes are uniform*. The
   `virt_dma_alloc` API is still required so the call sites remain
   valid in M2.4.

### M2.4+: MMU on, kernel RAM as Normal Inner-Shareable Cacheable

When M2.4 enables the MMU and re-maps kernel RAM as Normal cacheable,
the rules become the standard non-coherent-DMA rules:

1. Before the device reads a guest-written buffer, the guest must
   `dc cvac` (clean to PoC) the lines it touched, then DMB.
2. Before the guest reads a device-written buffer, it must `dc ivac`
   (invalidate) the lines, then DMB. Skipping the invalidate risks
   reading stale cached values that pre-date the device write.
3. Buffers that round-trip in both directions need clean+invalidate
   (`dc civac`) at the appropriate moments.
4. Allocator-provided RAM is what the driver must use. Stack and
   `kheap` allocations have no guaranteed cache attributes for DMA
   purposes; they may be cacheable but their lifetime is wrong for
   DMA round-trips.

The transition is built into the same helpers — see "What changes in
M2.4" below.

## Barrier rules

Use the helpers in `kernel/arch/arm64/dma.h`:

| Helper | When to use |
|---|---|
| `arm64_dma_wmb()` | After writing descriptors, before publishing the descriptor index to the device. After publishing the descriptor index, before the MMIO kick that wakes the device. |
| `arm64_dma_rmb()` | After reading the device's progress counter (e.g. `used->idx`), before reading the data the device wrote behind it. |
| `arm64_dma_mb()`  | At MMIO transitions where partial ordering is unsafe. Prefer `wmb` / `rmb` when the direction is known. |

Worked example — virtio split-ring submission
(see `kernel/arch/arm64/platform/virt/virtio_queue.c`):

```c
q->avail->ring[slot] = head;
arm64_dma_wmb();         /* descriptor + ring write retire first */
q->avail->idx++;
arm64_dma_wmb();         /* avail->idx retires before MMIO kick */
mmio_write32(base + VIRTIO_MMIO_QUEUE_NOTIFY, 0u);
```

Worked example — virtio used-ring drain:

```c
arm64_dma_rmb();          /* observe used->idx and the ring entries that
                           * back it consistently */
used_idx = q->used->idx;
if (used_idx == q->last_used)
    return NO_COMPLETION;
elem = q->used->ring[q->last_used & MASK];
```

## Cache-maintenance rules

In M2.x the helpers `arm64_dma_cache_clean()` and
`arm64_dma_cache_invalidate()` compile to nothing on purpose; calling
them is harmless. Drivers should already call them at the right
points so that M2.4's bodies (`dc cvac` and `dc ivac`) flip on
without touching call sites.

Convention:

```c
/* Before the device reads a guest-written buffer. */
fill_buffer(buf, len);
arm64_dma_cache_clean(buf, len);
arm64_dma_wmb();
publish_descriptor(...);

/* After the device wrote a buffer the guest will read. */
wait_for_completion();
arm64_dma_cache_invalidate(buf, len);
arm64_dma_rmb();
read_buffer(buf, len);
```

For round-trip buffers (e.g. virtio-blk request headers that are
RO from the device's view but the guest modifies again on the next
request), call `arm64_dma_cache_clean` before publishing each new
request — the buffer has no device-written contents to invalidate.

## Allocator contract

Use `virt_dma_alloc()` from `kernel/arch/arm64/platform/virt/dma.h`
for any buffer that will be handed to a virtio device. The allocator
guarantees:

- Page-aligned, contiguous physical pages.
- Identity-mapped today (M2.x): a `uintptr_t` cast to `uint64_t`
  equals the device-visible physical address.
- Stable address translation through `virt_virt_to_phys()` and
  `virt_phys_to_virt()`. Use these wrappers rather than raw casts
  so M2.4's heap-backed allocator can change the mapping without
  touching drivers.

The allocator does not perform barriers or cache maintenance. That
is the caller's job, per the rules above.

Stack and `kheap` allocations are not DMA-safe in M2.x because their
cache attributes are unknown to the driver. Allocate from
`virt_dma_alloc` even if the values look interchangeable today.

## What changes in M2.4

When M2.4 enables the MMU and Normal cacheable kernel RAM, three
things change at once and only inside `kernel/arch/arm64/dma.h` and
`kernel/arch/arm64/platform/virt/dma.c`:

1. `arm64_dma_cache_clean(addr, len)` becomes a `dc cvac` loop over
   `[addr, addr+len)` followed by `dsb ish`.
2. `arm64_dma_cache_invalidate(addr, len)` becomes a `dc ivac` loop
   followed by `dsb ish`.
3. `virt_dma_alloc` may move from a static BSS pool to a kheap-backed
   contiguous-page allocator. The API is unchanged.

Drivers do not change.

The M2.4 commit must verify that every existing virtio driver still
passes its smoke and KTEST coverage with the cache-maintenance bodies
turned on. A driver that was implicitly relying on the M2.x no-op
will fail this check, and the fix is at the driver site — that is the
whole point of the "drivers call these helpers in M2.x even though
they do nothing" discipline.

## References

- ARM Architecture Reference Manual (Armv8-A) §B2.3 (memory ordering),
  §B2.7 (cache maintenance).
- Virtio 1.0 §2.4 (split-ring layout) and §2.6 (memory ordering).
- `kernel/arch/arm64/dma.h` — barrier and cache-maintenance helpers.
- `kernel/arch/arm64/platform/virt/dma.h` — DMA-safe page allocator.
- `kernel/arch/arm64/platform/virt/virtio_queue.c` — worked example.
- `kernel/arch/arm64/platform/virt/virtio_blk.c` — worked example.
