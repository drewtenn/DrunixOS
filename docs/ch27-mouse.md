\newpage

## Chapter 27 — Mouse Input

### From a Movement on the Desk to a Pointer Event

Chapter 26 completed the memory story. The kernel can fork, exec, grow heaps and stacks on demand, and share pages until the first write. It already has one human-input device from Chapter 10 — the keyboard — feeding characters into a ring buffer one keystroke at a time. What it does not yet have is a *pointing* device. This chapter adds the PS/2 mouse, and it does so with more machinery than the keyboard needed: a second interrupt line, a multi-byte packet format that has to be reassembled across interrupts, and a coalescing strategy for fast motion.

The life of a single mouse event — a nudge on the desk, a click, a drag — runs through seven steps before it reaches the desktop:

1. The user moves or clicks. The mouse reports the change as a three-byte packet on the **PS/2 auxiliary channel** — the second channel of the same 8042 keyboard controller we already use for keystrokes.
2. The controller raises **IRQ12**, a separate interrupt line dedicated to the auxiliary device.
3. IRQ12 lives on the slave PIC (the one that handles IRQ8–15), which forwards the signal through the master PIC to the CPU. The CPU vectors into our mouse IRQ handler through the IDT and the common IRQ dispatch path from Chapter 5.
4. The handler reads as many bytes as the controller has queued — on a fast drag that can be several packets in one interrupt — and feeds each byte into a small state machine that reassembles three-byte packets.
5. Each completed packet is decoded into a **pointer event** containing signed motion deltas and a button snapshot.
6. The handler updates a global pointer position in pixel coordinates, clamped to the screen bounds.
7. The event is handed to the desktop compositor through a single callback. The compositor decides what to do with it — moving the on-screen cursor, forwarding the click to a window, starting a drag — and that is Chapter 28's subject.

Between steps 1 and 7 the rest of the system is passive. The mouse driver, like the keyboard driver, is one of the few places in the kernel where execution flows from hardware up into software rather than the other way around.

### The PS/2 Auxiliary Channel

A PS/2 keyboard controller — the 8042 chip, named for the part number used in the original IBM PC/AT — has two device channels. The **primary channel** carries keyboard traffic and raises IRQ1, which Chapter 10's driver already owns. The **auxiliary channel** is physically identical but routes its traffic through a second output buffer and raises its own interrupt, IRQ12.

Both channels share the same I/O ports:

- Port `0x60` is the **data port**. Reading it returns the most recently received byte from whichever device the controller last buffered.
- Port `0x64` doubles as the **status port** (when read) and the **command port** (when written).

Two status bits matter for mouse work. Bit 0 (`PS2_STATUS_OUT_FULL`) is set when the controller has a byte ready for us to read on port `0x60`. Bit 5 (`PS2_STATUS_AUX_BUFFER`) is set when that byte came from the auxiliary channel rather than the keyboard — the flag that lets a mouse interrupt distinguish "this data is for me" from "this data is for the keyboard driver".

### Enabling the Auxiliary Channel

The auxiliary channel starts disabled. Before any mouse packets will arrive, the driver has to walk through a short initialisation dance:

1. **Disable the aux channel briefly** (controller command `0xA7`) and flush any stale bytes sitting in the output buffer, entering a known state.
2. **Edit the controller's configuration byte.** Command `0x20` reads it; command `0x60` writes it back. Setting bit 1 enables the aux-interrupt path so that bytes arriving on the auxiliary channel actually raise IRQ12.
3. **Re-enable the aux channel** (command `0xA8`).
4. **Tell the mouse device itself to enter stream mode.** Device commands are sent through the "write-to-aux" escape: controller command `0xD4`, then the data byte. Command `0xF4` switches the mouse into **stream mode**, in which it reports motion and button changes automatically as they happen.

Each write to the mouse device is followed by a one-byte acknowledgement. `0xFA` means "accepted"; `0xFE` means "resend". Our driver treats anything other than `0xFA` as initialisation failure.

The driver also unmasks two IRQs at the PIC. IRQ12 is the obvious one — the mouse's own line. IRQ2 is the less obvious one: it is the **cascade line** that lets the slave PIC forward its interrupts to the master. Without IRQ2 unmasked, IRQ12 would fire on the slave but never reach the CPU. The mouse is the first device we add on the slave PIC, so this is the first time the cascade matters.

### The Three-Byte Packet Format

In stream mode, every movement or button change produces a fresh three-byte packet. The first byte encodes button state and flags; the second and third carry the signed motion deltas.

| Byte | Bits 7–4 | Bit 3 | Bits 2–0 |
|------|----------|-------|----------|
| 0 | Y overflow (7), X overflow (6), Y sign (5), X sign (4) | Always 1 | Middle (2), Right (1), Left (0) |
| 1 | Signed 8-bit X movement delta | | |
| 2 | Signed 8-bit Y movement delta | | |

A few of those fields deserve explanation.

The **"always 1" bit** in the first byte is a synchronisation sentinel. Every valid first byte of a packet has this bit set. If the driver ever gets out of phase with the device — a byte was lost, or the driver started listening mid-packet — it can realign by discarding bytes until it sees one with bit 3 set and treating that as a fresh first byte.

The **X and Y sign bits** extend the 8-bit delta byte into a signed 9-bit value. The delta byte itself is a two's-complement 8-bit integer, and the sign bit in the flags byte carries the ninth bit — not a redundant sign copy, but the overflow direction when motion exceeded the 8-bit range.

The **X and Y overflow bits** mean the real motion exceeded even the 9-bit range. When either overflow bit is set, the delta byte value is meaningless — the hardware has already clipped it — and the driver replaces it with `±127` based on the sign flag so the compositor at least sees "full scale in this direction" rather than a truncated fragment.

Note that the Y axis in PS/2 coordinates runs *upward* — positive Y means the mouse moved away from the user. Screen coordinates, by contrast, run downward: y=0 is the top of the display. The driver flips the sign when applying the Y delta to the on-screen pointer.

### Packet Reassembly in the IRQ Handler

Each IRQ delivers exactly one byte from the controller's output buffer. Three IRQs make a complete packet, so the driver keeps a tiny state machine:

```c
typedef struct {
    uint8_t bytes[3];
    int index;
} mouse_packet_stream_t;
```

The `index` field tracks how many bytes we have received so far in the current packet. When a byte arrives and `index` is zero, the driver checks the sentinel bit: if the "always 1" bit is clear, the byte cannot be the start of a valid packet and is discarded. Otherwise the byte is stored and `index` advances. When three bytes have been collected, the packet is handed off for decoding and the state is reset.

This sentinel-bit check is what makes the stream self-synchronising. Even if the driver started up mid-stream, or a byte was dropped during a spurious interrupt, it rejects misaligned bytes until the next "always 1" aligns the stream again.

### Coalescing Motion Packets

One subtle cost matters on fast movement. When the user flings the mouse across the desk, the 8042 can queue several complete packets between IRQs. A naive "one packet per IRQ" handler runs the full render pipeline once per packet, which on a 1024×768 framebuffer is expensive enough to feel in a drag.

Our handler drains every queued byte inside a single interrupt. That is why the read loop consults both `PS2_STATUS_OUT_FULL` *and* `PS2_STATUS_AUX_BUFFER`: once three bytes have been consumed (a full packet for us), we keep reading only while the controller's next queued byte is also from the aux channel. A byte from the keyboard would stop the loop and leave the keystroke for the keyboard IRQ to pick up.

Within that loop, *motion-only* packets are coalesced. The handler keeps the most recent pending pointer event and overwrites it every time another pure-motion packet arrives. Only when a **button edge** appears — a press or a release, detected by comparing the new button state against the previous one — does the handler flush the pending motion event first and then dispatch the edge event immediately, unmerged.

That split matters because the compositor's drag-state machine needs every press and every release. Losing a release by merging it with later motion would leave the compositor in "still dragging" mode after the user had let go of the button. Coalescing only the pure-motion intermediates keeps the edge events honest while avoiding a render per packet.

### Pointer Position and Acceleration

Motion deltas arrive in PS/2 units — roughly one pixel per unit at the mouse's reporting rate. On a 1024×768 framebuffer that produces a sluggish, twitchy cursor: the user has to slide the mouse a long way across the desk to cross the screen. The driver multiplies the delta by a scale factor before applying it:

```c
g_pointer_pixel_x += ev->dx * scale;
g_pointer_pixel_y -= ev->dy * scale;
```

The `scale` value is chosen at compile time through `MOUSE_FRAMEBUFFER_PIXEL_SCALE`, clamped to the range 1–16. The default of 4 gives a comfortable desktop feel on the default framebuffer mode; the build system exposes it as the `MOUSE_SPEED` variable. Legacy VGA text mode (80×25 cells, or 640×400 pixels) uses a scale of 1, since the much smaller effective resolution makes acceleration unnecessary.

Each update is clamped to the framebuffer bounds so the cursor cannot wander off-screen. The clamped pixel coordinates are written back to the event structure along with character-cell coordinates computed by dividing by the 8×16 glyph size. The compositor uses either form depending on where the event lands: pixel coordinates for hit-testing window chrome, cell coordinates for routing input into the terminal cell grid.

### Handing Events to the Next Layer

The final step is notifying the desktop. Each fully-decoded, position-updated pointer event is handed to a single entry point:

```c
desktop_handle_pointer(desktop, &ev);
```

The compositor receives a plain data structure — pixel and character-cell position, motion deltas, current left-button state — and decides what to do with it. The mouse driver has no knowledge of windows, focus, or which app should receive the click. Its job ends at "the pointer is here; the left button is down".

This mirrors the keyboard driver's architecture from Chapter 10: the producer (an IRQ handler) is strictly hardware-facing, and the consumer (a higher-level subsystem) is strictly policy-facing. Neither side needs to know the other's internals.

### Where the Machine Is by the End of Chapter 27

The kernel now has a second human-input device with the keyboard's interrupt-driven shape plus the machinery a pointing device needs: a synchronising packet stream, a motion-coalescing IRQ loop, and a scaled, clamped global pointer position. Every mouse movement or click results in exactly one `desktop_handle_pointer` call per semantic event — many motion samples collapse into a single update, but no button edge is ever lost. The compositor on the other side of that call is still a stub. Chapter 28 is where it becomes a real windowed desktop.
