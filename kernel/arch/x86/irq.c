/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * irq.c — IRQ handler registration and dispatch glue for hardware interrupts.
 */

#include "irq.h"
#include "io.h"
#include "klog.h"
#include "irq_table.h"

#define PIC_MASTER_CMD 0x20
#define PIC_SLAVE_CMD 0xA0
#define PIC_MASTER_DATA 0x21
#define PIC_SLAVE_DATA 0xA1
#define PIC_EOI 0x20

static irq_handler_generic_fn irq_table[IRQ_COUNT];
static uint8_t g_master_pic_mask = 0xFC;
static uint8_t g_slave_pic_mask = 0xFF;

static void irq_pic_write_masks(void)
{
	port_byte_out(PIC_MASTER_DATA, g_master_pic_mask);
	port_byte_out(PIC_SLAVE_DATA, g_slave_pic_mask);
}

void irq_dispatch_init(void)
{
	irq_table_clear(irq_table, IRQ_COUNT);
}

void irq_set_pic_masks(uint8_t master_mask, uint8_t slave_mask)
{
	g_master_pic_mask = master_mask;
	g_slave_pic_mask = slave_mask;
	irq_pic_write_masks();
}

void irq_apply_pic_masks(void)
{
	irq_pic_write_masks();
}

void irq_unmask(uint8_t irq_num)
{
	if (irq_num < 8) {
		g_master_pic_mask &= (uint8_t)~(1u << irq_num);
	} else if (irq_num < IRQ_COUNT) {
		g_slave_pic_mask &= (uint8_t)~(1u << (irq_num - 8));
		g_master_pic_mask &= (uint8_t)~(1u << 2);
	} else {
		return;
	}
	irq_pic_write_masks();
}

void irq_mask(uint8_t irq_num)
{
	if (irq_num < 8) {
		g_master_pic_mask |= (uint8_t)(1u << irq_num);
	} else if (irq_num < IRQ_COUNT) {
		g_slave_pic_mask |= (uint8_t)(1u << (irq_num - 8));
	} else {
		return;
	}
	irq_pic_write_masks();
}

void irq_register(uint8_t irq_num, irq_handler_fn fn)
{
	irq_table_set(irq_table, IRQ_COUNT, irq_num, (irq_handler_generic_fn)fn);
}

void irq_dispatch(uint32_t vector, uint32_t error_code)
{
	(void)error_code;

	uint8_t irq_num = (uint8_t)(vector - 32);
	irq_handler_generic_fn fn = irq_table_get(irq_table, IRQ_COUNT, irq_num);
	if (fn)
		fn();

	/* Send EOI to master PIC; also send to slave for IRQ lines 8–15. */
	if (irq_num >= 8)
		port_byte_out(PIC_SLAVE_CMD, PIC_EOI);
	port_byte_out(PIC_MASTER_CMD, PIC_EOI);
}
