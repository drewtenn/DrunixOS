/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KERNEL_ARCH_ARM64_USB_KEYBOARD_H
#define KERNEL_ARCH_ARM64_USB_KEYBOARD_H

int arm64_usb_keyboard_init(void);
void arm64_usb_keyboard_poll(void);

#endif
