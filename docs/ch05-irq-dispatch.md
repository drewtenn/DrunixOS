\newpage

## Chapter 5 — The IRQ Dispatch Registry

### One Handler Can't Scale

Chapter 4 ended with the IDT live and the CPU able to handle interrupts and exceptions. Every hardware IRQ funnelled into a single C function inside the keyboard driver. That function contained a chain of `if`/`else if` checks: if the vector number is 32 call the scheduler tick; if it is 33 call the keyboard handler. Adding a third device meant extending the chain by hand. The keyboard code had become an unrelated dependency for every future driver.

A more important problem is that the IRQ numbers — 32 for the PIT timer and 33 for the keyboard — were baked into the logic of a file nominally about the keyboard. Any developer adding a serial port driver would need to know the exact vector numbers used by two completely different subsystems before they could touch anything.

The IRQ dispatch registry solves both problems at once. Instead of one global handler, we keep a table of 16 function pointers, indexed by IRQ line number (zero-based, so IRQ 0 is the PIT timer and IRQ 1 is the PS/2 keyboard). Each driver claims its own IRQ line during startup. The common IRQ path converts the raw vector number to an IRQ line index, looks up the registered function pointer, and calls it.

### The Full IRQ Path

The path from a hardware event to a registered C handler now looks like this:

![](diagrams/ch05-diag01.svg)

### The Registry Structure

The dispatch table is a static array of 16 `irq_handler_fn` function pointers, all initialised to NULL during early startup. The type is:

```c
typedef void (*irq_handler_fn)(void);
```

Each entry corresponds to one of the 16 IRQ lines the 8259A **PIC** (Programmable Interrupt Controller) can deliver. IRQ lines 0–7 are handled by the master PIC and arrive at CPU vectors 32–39. Lines 8–15 are handled by the slave PIC and arrive at vectors 40–47. The table uses the zero-based line index because that is stable across different PIC remapping configurations.

### Registering a Handler

A driver registers its handler during init, before interrupts are enabled. The registration stores one function pointer in the slot for that IRQ line. If a second driver claims the same line, the new handler replaces the old one — there is no chain.

| Slot | Handler | Purpose |
|------|---------|---------|
| `0` | Timer handler | Handles timer ticks |
| `1` | Keyboard handler | Handles keyboard input |
| `2..14` | None | No handler registered yet |
| `15` | None | No handler registered yet |

### The Dispatch Function

The common IRQ dispatch step receives the raw IDT vector number (32–47) and a dummy error code (always zero for hardware IRQs, pushed by the stub). It then:

1. Computes `irq_num = vector - 32`.
2. If `irq_num < 16` and `irq_table[irq_num]` is non-NULL, calls the registered function.
3. Sends an **EOI** (End of Interrupt) to the PIC. For IRQ lines 8–15 the EOI goes to both the slave PIC (port `0xA0`) and the master PIC (port `0x20`). For lines 0–7 only the master receives it.

The EOI step is centralised here so individual handlers do not need to perform it. A driver that forgets to send EOI would leave the PIC stuck and prevent all future interrupts of the same priority. By placing it in the dispatch layer, we guarantee EOI always goes out, regardless of what the handler does.

### The PIT Timer Handler

The **PIT** (Programmable Interval Timer, the Intel 8253/8254 chip) generates the periodic IRQ 0 that drives the scheduler's timeslice counter. Before this chapter, the scheduler tick was called directly from the keyboard driver's IRQ handler, an odd placement. Now the timer driver owns IRQ 0. During kernel startup, before interrupts are enabled, it claims that line with a handler that does two things in sequence: advances the wall clock by one tick, then nudges the scheduler so it can decide whether to preempt the running process.

The PIT's firing frequency is fixed during interrupt bring-up. The PIT's internal oscillator runs at 1,193,182 Hz. Writing a divisor of 11 932 to the channel-0 reload register — command byte `0x36` to port `0x43`, then the low and high bytes of 11 932 to port `0x40` — configures the chip to fire in mode 2 (rate generator), producing approximately 100 interrupts per second. One IRQ 0 therefore arrives roughly every 10 milliseconds, which is the scheduler's timeslice granularity.

### The Wall Clock

Every real Unix system needs a notion of what time it is right now — a value that survives power cycles and can be read cheaply at any point during execution. On x86 the answer is the **CMOS RTC** (Complementary Metal-Oxide Semiconductor Real-Time Clock), a battery-backed chip that keeps ticking even when the machine is off. We read it exactly once at boot and then maintain a software counter from that seed.

#### Reading the CMOS RTC

The RTC is exposed through two I/O ports: address port `0x70` and data port `0x71`. Writing a register number to `0x70` selects the register; reading `0x71` returns its value. The relevant registers are:

| Register | Address | Contents |
|---|---|---|
| Seconds | `0x00` | current second (0–59) |
| Minutes | `0x02` | current minute (0–59) |
| Hours | `0x04` | current hour (0–23 or 12-hour + PM bit) |
| Day of month | `0x07` | day (1–31) |
| Month | `0x08` | month (1–12) |
| Year | `0x09` | year modulo 100 |
| Status A | `0x0A` | bit 7 = update-in-progress flag |
| Status B | `0x0B` | bit 2 = binary mode; bit 1 = 24-hour mode |

The RTC updates its registers approximately once per second, and during the update window any individual register read may return a partially incremented value. We avoid this by reading the entire set twice and comparing. If both reads agree, the values are stable. If they disagree, or if the status A update-in-progress flag is set at either sample, the loop retries. After up to 100 000 tries the read is abandoned and the clock is left at zero, which means the system has no wall time — a degraded but safe state.

After a stable sample is in hand, the kernel converts the raw register values to binary. By default the CMOS stores values in **BCD** (Binary-Coded Decimal), where each decimal digit occupies one nibble of the byte — for example, the decimal value 42 is stored as `0x42`. Unless Status B bit 2 is set (indicating binary mode), each field is decoded from BCD into an ordinary integer. The 12/24-hour format is also normalised: if Status B bit 1 is clear, the RTC is in 12-hour mode and the PM indicator is encoded in the top bit of the hours byte.

Once all six fields are in calendar form, they are converted to a **Unix timestamp** — the number of seconds elapsed since 1970-01-01 00:00:00 UTC. The conversion counts the number of days from 1970 to the current year (accounting for leap years with the Gregorian rule: divisible by 4 but not 100, or divisible by 400), adds the days elapsed in the current year up to the current month and day, and then multiplies total days by 86 400 before adding the hours, minutes, and seconds. The result is stored in a static `g_unix_time` variable.

#### Advancing the Clock on PIT Ticks

Reading the RTC every second would be possible but wasteful. Instead, the PIT fires at 100 Hz and the timer path increments a sub-second tick counter on every interrupt. When that counter reaches 100, it resets to zero and increments the Unix-time counter by one. This way the wall clock advances at exactly one second per 100 PIT ticks — the same rate as the scheduler.

We expose the current Unix-time counter to user space through `SYS_CLOCK_GETTIME`. Because the CMOS is only read once, the clock drifts over long runtimes; for a system with no **NTP** (Network Time Protocol — the internet standard for synchronising clocks across machines) and a battery-backed RTC that is set accurately at boot, the drift is small enough to be irrelevant.

![](diagrams/ch05-diag02.svg)

### The New Boot Sequence Excerpt

After this chapter, the interrupt-related portion of kernel startup proceeds in this order:

1. The IRQ dispatch table is cleared so no slot contains stale garbage that could be accidentally invoked.
2. The timer driver registers its handler for IRQ 0.
3. The ATA disk driver probes the drive.
4. The keyboard driver registers its handler for IRQ 1.
5. The IDT is filled, the PIC is remapped, the timer is programmed to 100 Hz, and interrupts are finally enabled.

Every registration completes before interrupts are turned on, which ensures the table is fully consistent before any IRQ can arrive.

### Where the Machine Is by the End of Chapter 5

We now have a proper interrupt dispatch mechanism. Any driver can register for any IRQ line without knowing what other drivers have registered. The PIT and keyboard handlers are installed through the same registry as any future driver, and the centralised EOI means no driver can accidentally leave the PIC in a locked state by forgetting the End of Interrupt write.

The system also has a wall clock seeded from the CMOS RTC at boot and maintained by the PIT interrupt at 100 Hz. User programs can read the current Unix time through `SYS_CLOCK_GETTIME` without ever touching a hardware port directly.
