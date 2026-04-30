/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_INPUT_H
#define KERNEL_ARCH_ARM64_PLATFORM_VIRT_VIRTIO_INPUT_H

/*
 * Phase 1 M2.5b: virtio-input driver for the QEMU virt machine.
 *
 * Scans virtio-mmio for DeviceID 18 instances, reads each one's name
 * via config space, and registers an eventq virtqueue per device.
 * Keyboard events route to kbdev_push_event(); mouse events route to
 * mousedev_push_event(). Events arrive in Linux evdev format
 * (type/code/value) so they pass through to /dev/kbd and /dev/mouse
 * essentially unchanged — Linux KEY_* values match PS/2 set-1 scan
 * codes for the basic typing range, so user/apps/desktop's existing
 * kbdmap path keeps working.
 *
 * Returns the number of virtio-input devices that registered
 * successfully (0..N), or -1 on a hard failure that should be logged.
 * Tolerates the absence of any virtio-input device (KTEST builds
 * without -device virtio-{keyboard,mouse}-device): the function logs
 * and returns 0.
 */
int arm64_virt_input_register_all(void);

#endif
