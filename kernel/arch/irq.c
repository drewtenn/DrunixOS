/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c — IRQ handler registration and dispatch glue for hardware interrupts.
 */

#include "irq.h"
#include "klog.h"

#define PIC_MASTER_CMD  0x20
#define PIC_SLAVE_CMD   0xA0
#define PIC_EOI         0x20

extern void port_byte_out(unsigned short port, unsigned char data);

static irq_handler_fn irq_table[IRQ_COUNT];

void irq_dispatch_init(void) {
    for (int i = 0; i < IRQ_COUNT; i++)
        irq_table[i] = 0;
}

void irq_register(uint8_t irq_num, irq_handler_fn fn) {
    if (irq_num < IRQ_COUNT)
        irq_table[irq_num] = fn;
}

void irq_dispatch(uint32_t vector, uint32_t error_code) {
    (void)error_code;

    uint8_t irq_num = (uint8_t)(vector - 32);
    if (irq_num < IRQ_COUNT && irq_table[irq_num])
        irq_table[irq_num]();

    /* Send EOI to master PIC; also send to slave for IRQ lines 8–15. */
    if (irq_num >= 8)
        port_byte_out(PIC_SLAVE_CMD, PIC_EOI);
    port_byte_out(PIC_MASTER_CMD, PIC_EOI);
}
