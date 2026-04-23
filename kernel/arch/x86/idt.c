/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * idt.c — IDT setup, delayed interrupt enablement, and fatal exception
 * handling.
 */

#include "idt.h"
#include "fault.h"
#include "gdt.h"
#include "io.h"
#include "irq.h"
#include "sched.h"
#include <stdint.h>

#define IDT_ENTRIES 256
#define KERNEL_CS 0x08 /* CODE_SEG: gdt_code - gdt_start */

/* 8-byte interrupt gate descriptor */
typedef struct {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t zero;
	uint8_t type_attr; /* 0x8E = present, ring 0, 32-bit interrupt gate */
	uint16_t offset_high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed)) idt_register_t;

static idt_entry_t idt[IDT_ENTRIES];
static idt_register_t idtr;

/* ISR stubs (defined in isr.asm) */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

/* Syscall stub */
extern void isr128(void);

/* IRQ stubs */
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

static void idt_set_gate(uint8_t vector, uint32_t handler)
{
	idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
	idt[vector].selector = KERNEL_CS;
	idt[vector].zero = 0;
	idt[vector].type_attr = 0x8E;
	idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
}

static void idt_set_task_gate(uint8_t vector, uint16_t tss_selector)
{
	idt[vector].offset_low = 0;
	idt[vector].selector = tss_selector;
	idt[vector].zero = 0;
	idt[vector].type_attr = 0x85; /* present, DPL=0, 32-bit task gate */
	idt[vector].offset_high = 0;
}

void isr_handler(arch_trap_frame_t *f);

static void pic_remap(void)
{
	/* ICW1: start initialization, will send ICW4 */
	port_byte_out(0x20, 0x11);
	port_byte_out(0xA0, 0x11);
	/* ICW2: vector offsets — master IRQ0-7 → vectors 32-39, slave IRQ8-15 → 40-47 */
	port_byte_out(0x21, 0x20);
	port_byte_out(0xA1, 0x28);
	/* ICW3: cascade wiring — slave on master IR2 */
	port_byte_out(0x21, 0x04);
	port_byte_out(0xA1, 0x02);
	/* ICW4: 8086 mode */
	port_byte_out(0x21, 0x01);
	port_byte_out(0xA1, 0x01);
	/* Apply the current PIC mask state after remapping the IRQ vectors. */
	irq_apply_pic_masks();
}

void idt_init_early(void)
{
	/* CPU exceptions */
	idt_set_gate(0, (uint32_t)isr0);
	idt_set_gate(1, (uint32_t)isr1);
	idt_set_gate(2, (uint32_t)isr2);
	idt_set_gate(3, (uint32_t)isr3);
	idt_set_gate(4, (uint32_t)isr4);
	idt_set_gate(5, (uint32_t)isr5);
	idt_set_gate(6, (uint32_t)isr6);
	idt_set_gate(7, (uint32_t)isr7);
	idt_set_task_gate(8, GDT_DF_TSS_SEG);
	idt_set_gate(9, (uint32_t)isr9);
	idt_set_gate(10, (uint32_t)isr10);
	idt_set_gate(11, (uint32_t)isr11);
	idt_set_gate(12, (uint32_t)isr12);
	idt_set_gate(13, (uint32_t)isr13);
	idt_set_gate(14, (uint32_t)isr14);
	idt_set_gate(15, (uint32_t)isr15);
	idt_set_gate(16, (uint32_t)isr16);
	idt_set_gate(17, (uint32_t)isr17);
	idt_set_gate(18, (uint32_t)isr18);
	idt_set_gate(19, (uint32_t)isr19);
	idt_set_gate(20, (uint32_t)isr20);
	idt_set_gate(21, (uint32_t)isr21);
	idt_set_gate(22, (uint32_t)isr22);
	idt_set_gate(23, (uint32_t)isr23);
	idt_set_gate(24, (uint32_t)isr24);
	idt_set_gate(25, (uint32_t)isr25);
	idt_set_gate(26, (uint32_t)isr26);
	idt_set_gate(27, (uint32_t)isr27);
	idt_set_gate(28, (uint32_t)isr28);
	idt_set_gate(29, (uint32_t)isr29);
	idt_set_gate(30, (uint32_t)isr30);
	idt_set_gate(31, (uint32_t)isr31);

	/* Hardware IRQs (remapped to vectors 32-47) */
	idt_set_gate(32, (uint32_t)irq0);
	idt_set_gate(33, (uint32_t)irq1);
	idt_set_gate(34, (uint32_t)irq2);
	idt_set_gate(35, (uint32_t)irq3);
	idt_set_gate(36, (uint32_t)irq4);
	idt_set_gate(37, (uint32_t)irq5);
	idt_set_gate(38, (uint32_t)irq6);
	idt_set_gate(39, (uint32_t)irq7);
	idt_set_gate(40, (uint32_t)irq8);
	idt_set_gate(41, (uint32_t)irq9);
	idt_set_gate(42, (uint32_t)irq10);
	idt_set_gate(43, (uint32_t)irq11);
	idt_set_gate(44, (uint32_t)irq12);
	idt_set_gate(45, (uint32_t)irq13);
	idt_set_gate(46, (uint32_t)irq14);
	idt_set_gate(47, (uint32_t)irq15);

	/*
     * INT 0x80 syscall gate.
     *
     * type_attr = 0xEF:
     *   bit 7    = 1 (present)
     *   bits 6-5 = 11 (DPL=3 — ring-3 code may invoke this vector)
     *   bit 4    = 0 (system descriptor)
     *   bits 3-0 = 1111 (32-bit trap gate — does NOT clear IF on entry,
     *                     so hardware interrupts stay enabled during syscalls)
     *
     * Using 0x8E (ring-0 interrupt gate) here would cause an immediate #GP
     * when user code executes "int $0x80", because the CPU checks that the
     * caller's CPL <= gate DPL before allowing software-triggered vectors.
     */
	idt[128].offset_low = (uint16_t)((uint32_t)isr128 & 0xFFFF);
	idt[128].selector = KERNEL_CS;
	idt[128].zero = 0;
	idt[128].type_attr = 0xEF;
	idt[128].offset_high = (uint16_t)(((uint32_t)isr128 >> 16) & 0xFFFF);

	idtr.limit = (uint16_t)(sizeof(idt_entry_t) * IDT_ENTRIES - 1);
	idtr.base = (uint32_t)&idt[0];
	__asm__ volatile("lidt (%0)" : : "r"(&idtr));
}

void interrupts_enable(void)
{
	pic_remap();
	__asm__ volatile("sti");
}

static const char *exception_name(uint32_t vec)
{
	static const char *names[] = {
	    "#DE Divide Error",
	    "#DB Debug",
	    "#NMI NMI Interrupt",
	    "#BP Breakpoint",
	    "#OF Overflow",
	    "#BR BOUND Range Exceeded",
	    "#UD Invalid Opcode",
	    "#NM Device Not Available",
	    "#DF Double Fault",
	    "#MF Coprocessor Overrun",
	    "#TS Invalid TSS",
	    "#NP Segment Not Present",
	    "#SS Stack-Segment Fault",
	    "#GP General Protection Fault",
	    "#PF Page Fault",
	    "#15 Reserved",
	    "#MF x87 FPU Error",
	    "#AC Alignment Check",
	    "#MC Machine Check",
	    "#XM SIMD FPU Exception",
	    "#VE Virtualisation",
	    "#21 Reserved",
	};
	if (vec < 22)
		return names[vec];
	return "Unknown Exception";
}

#define COM1_PORT 0x3F8
#define COM1_LSR (COM1_PORT + 5)
#define QEMU_DEBUG_PORT 0xE9

static int panic_serial_ready = 0;

static void panic_serial_init(void)
{
	if (panic_serial_ready)
		return;

	port_byte_out(COM1_PORT + 1, 0x00);
	port_byte_out(COM1_PORT + 3, 0x80);
	port_byte_out(COM1_PORT + 0, 0x03);
	port_byte_out(COM1_PORT + 1, 0x00);
	port_byte_out(COM1_PORT + 3, 0x03);
	port_byte_out(COM1_PORT + 2, 0xC7);
	port_byte_out(COM1_PORT + 4, 0x0B);
	panic_serial_ready = 1;
}

static void panic_putc(char c)
{
	if (!panic_serial_ready)
		panic_serial_init();

	if (c == '\n') {
		panic_putc('\r');
	}

	for (int i = 0; i < 10000; i++) {
		if (port_byte_in(COM1_LSR) & 0x20) {
			port_byte_out(COM1_PORT, (unsigned char)c);
			break;
		}
	}
	port_byte_out(QEMU_DEBUG_PORT, (unsigned char)c);
}

static void panic_puts(const char *s)
{
	while (*s)
		panic_putc(*s++);
}

static void panic_hex(const char *label, uint32_t val)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[32];
	int i = 0;

	buf[i++] = '[';
	buf[i++] = 'P';
	buf[i++] = 'A';
	buf[i++] = 'N';
	buf[i++] = 'I';
	buf[i++] = 'C';
	buf[i++] = ']';
	buf[i++] = ' ';
	for (int j = 0; label[j]; j++)
		buf[i++] = label[j];
	buf[i++] = '=';
	buf[i++] = '0';
	buf[i++] = 'x';
	for (int shift = 28; shift >= 0; shift -= 4)
		buf[i++] = hex[(val >> shift) & 0xF];
	buf[i++] = '\n';
	buf[i] = '\0';
	panic_puts(buf);
}

static int exception_signal(uint32_t vec)
{
	switch (vec) {
	case 0:
		return SIGFPE; /* #DE */
	case 1:
		return SIGTRAP; /* #DB */
	case 3:
		return SIGTRAP; /* #BP */
	case 6:
		return SIGILL; /* #UD */
	case 13:
		return SIGSEGV; /* #GP */
	case 14:
		return SIGSEGV; /* #PF */
	case 16:
		return SIGFPE; /* x87 floating point */
	case 17:
		return SIGSEGV; /* alignment check */
	case 19:
		return SIGFPE; /* SIMD floating point */
	default:
		return 0;
	}
}

/* Default exception handler — ring-3 faults become signals, ring-0 faults halt. */
void isr_handler(arch_trap_frame_t *f)
{
	if ((f->cs & 3) == 3) {
		uint32_t cr2 = 0;
		process_t *cur = sched_current();
		int signum = exception_signal(f->vector);

		if (f->vector == 14) {
			__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
			if (paging_handle_fault(cur ? cur->pd_phys : 0,
			                        cr2,
			                        f->error_code,
			                        f->user_esp,
			                        cur) == 0)
				return;
		}

		if (signum != 0) {
			sched_record_user_fault(f, cr2, signum);
			return;
		}
	}

	__asm__ volatile("cli");

	panic_puts("[PANIC] --- EXCEPTION ---\n");
	panic_puts("[PANIC] ");
	panic_puts(exception_name(f->vector));
	panic_puts("\n");

	/* Fault site */
	panic_hex("EIP", f->eip);
	panic_hex("CS ", f->cs);
	panic_hex("FLG", f->eflags);
	panic_hex("ERR", f->error_code);

	/* Ring level */
	if ((f->cs & 3) == 3) {
		panic_puts("[PANIC] ring=3 (user)\n");
		panic_hex("uESP", f->user_esp);
		panic_hex("uSS ", f->user_ss);
	} else {
		panic_puts("[PANIC] ring=0 (kernel)\n");
	}

	/* General-Purpose Fault error code decode (vector 13) */
	if (f->vector == 13 && f->error_code != 0) {
		uint32_t e = f->error_code;
		panic_puts("[PANIC] #GP selector detail follows\n");
		/* selector index is bits 15:3 */
		panic_hex("sel_idx", (e >> 3) & 0x1FFF);
		/* bit 1 = IDT, bit 0 = EXT */
		if (e & 2)
			panic_puts("[PANIC] source=IDT\n");
		else if (e & 4)
			panic_puts("[PANIC] source=LDT\n");
		else
			panic_puts("[PANIC] source=GDT\n");
	}

	/* Page Fault: CR2 holds the faulting linear address (vector 14) */
	if (f->vector == 14) {
		uint32_t cr2;
		__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
		panic_hex("CR2", cr2);
		/* PF error code bits: P=0 not-present, W=write, U=user */
		panic_puts("[PANIC] PF flags: ");
		if (!(f->error_code & 1))
			panic_puts("not-present ");
		if (f->error_code & 2)
			panic_puts("write ");
		if (f->error_code & 4)
			panic_puts("user ");
		panic_puts("\n");
	}

	/* General-purpose registers */
	panic_hex("EAX", f->eax);
	panic_hex("EBX", f->ebx);
	panic_hex("ECX", f->ecx);
	panic_hex("EDX", f->edx);
	panic_hex("ESI", f->esi);
	panic_hex("EDI", f->edi);
	panic_hex("EBP", f->ebp);

	/* Current process PID */
	panic_hex("PID", sched_current_pid());

	panic_puts("[PANIC] --- HALTED ---\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) void double_fault_task_entry(void)
{
	const tss_t *t = gdt_get_runtime_tss();
	uint32_t cr2 = 0;

	__asm__ volatile("cli");
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

	panic_puts("[PANIC] --- DOUBLE FAULT ---\n");
	panic_puts("[PANIC] fault entered through dedicated TSS\n");
	panic_hex("CR2", cr2);
	panic_hex("EIP", t->eip);
	panic_hex("ESP", t->esp);
	panic_hex("EBP", t->ebp);
	panic_hex("EAX", t->eax);
	panic_hex("EBX", t->ebx);
	panic_hex("ECX", t->ecx);
	panic_hex("EDX", t->edx);
	panic_hex("ESI", t->esi);
	panic_hex("EDI", t->edi);
	panic_hex("CS ", t->cs);
	panic_hex("SS ", t->ss);
	panic_hex("CR3", t->cr3);
	panic_hex("EFL", t->eflags);
	if ((t->cs & 3) == 0)
		panic_puts("[PANIC] prior context was ring=0 (kernel)\n");
	else
		panic_puts("[PANIC] prior context was ring=3 (user)\n");
	panic_puts("[PANIC] --- HALTED ---\n");

	for (;;)
		__asm__ volatile("cli; hlt");
}
