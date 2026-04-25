\newpage

## Chapter 19 — Signals

### What a Signal Is

Chapter 18 left us with a TTY that tracks a foreground process group and generates control-character events, but nothing to act on them yet. A **signal** is the kernel's way to inject control flow into a user process at a controlled boundary. Where an ordinary system call is a request the user process makes at a moment of its own choosing, a signal arrives from outside — from another process, from the kernel itself, or from a hardware event — at a moment the process did not choose and may not be prepared for. The kernel cannot simply jump into user code at a random instruction. Instead, it queues the signal, waits for the process to reach a safe transition point on its way back to user space, and then — and only then — performs a controlled injection: redirecting the return path so that user code lands inside a signal handler rather than wherever it was about to go.

Two parts of that picture are fully portable across architectures: the queueing of pending signals and the per-signal disposition table that records what the process wants to do with each signal. A third part — how the kernel actually forges the handler invocation on the user stack — depends on the architecture, because the exact shape of registers and stack frames differs between x86 and AArch64.

Linux defines signals using a standard numbering. We adopt the same numbers for the eight signals we support:

| Number | Name | Default action | When sent |
|--------|------|----------------|-----------|
| 2 | SIGINT | terminate | User presses Ctrl+C |
| 9 | SIGKILL | terminate (uncatchable) | `SYS_KILL` with signum 9 |
| 13 | SIGPIPE | terminate | Write to a broken pipe |
| 15 | SIGTERM | terminate | `SYS_KILL` with signum 15 |
| 17 | SIGCHLD | ignore | Child process exits or stops |
| 18 | SIGCONT | continue | Resumes a stopped process |
| 19 | SIGSTOP | stop (uncatchable) | `SYS_KILL` with signum 19 |
| 20 | SIGTSTP | stop | User presses Ctrl+Z |

Every process can choose to handle or ignore each signal (except SIGKILL and SIGSTOP, which cannot be caught, blocked, or ignored). Our signal subsystem has four jobs: recording that a signal is pending, choosing the moment to deliver it, killing the process or handing control to its handler, and — for the stop/continue pair — suspending or resuming execution.

### Signal State In Tasks And Thread Groups

Signal state is split between task-local fields and resources that may be shared by a thread group:

```c
uint32_t  sig_pending;           /* task-directed pending signals        */
uint32_t  sig_blocked;           /* task-local signal mask               */
uint32_t  sig_handlers[NSIG];    /* disposition table, shareable by clone */
```

**NSIG** (number of signals) is 32 — the size of a `uint32_t` bitmask. Bit N in a task's `sig_pending` means signal N has been sent to that specific task but not yet delivered. Bit N in the thread group's process-directed pending mask means signal N was sent to the process identity, the **TGID** (Thread Group ID, the shared identifier for all tasks in a process), rather than to a particular **TID** (Task ID, the unique identifier for a single task within the group). Bit N in `sig_blocked` means delivery of signal N is postponed for the current task until the bit is cleared.

Each entry in `sig_handlers` holds one of three values:

- `SIG_DFL` (0) — take the platform default action. For most signals this means terminate. SIGCHLD is ignored by default. SIGSTOP and SIGTSTP stop the process. SIGCONT resumes it.
- `SIG_IGN` (1) — silently discard the signal.
- Any other value — the virtual address of a user-space function with signature `void handler(int signum)`.

Signal state is initialized to all-zero in `process_create` (no pending signals, nothing blocked, every disposition SIG_DFL). `process_fork` copies `sig_handlers` from the parent so installed handlers survive across `fork`, but clears `sig_pending` in the child — POSIX specifies that pending signals are not inherited. `clone` with `CLONE_SIGHAND` shares the signal-disposition resource with the caller, while each task keeps its own signal mask and task-directed pending set.

### Sending a Signal

`sched_send_signal(pid, signum)` is the universal signal-sending function. If `pid` names a thread group, the signal becomes process-directed and is queued on the group; if it names an individual task, the signal is queued on that task. Waking uses the same helper as the rest of the scheduler, transitioning blocked tasks back to `PROC_READY` and clearing wait-queue linkage or timeout. Setting `g_need_switch = 1` ensures the scheduler runs at the next opportunity. This is why a sleeping process woken by a signal returns from `SYS_SLEEP` early, with the number of remaining seconds as its return value.

Two signals receive special treatment inside `sched_send_signal`:

- **SIGCONT**: if the target is in `PROC_STOPPED` state, it is immediately transitioned to `PROC_READY`. Any pending SIGSTOP or SIGTSTP bits are cleared — a continue cancels a queued stop (Linux semantics).
- **SIGSTOP / SIGTSTP**: any pending SIGCONT bit is cleared — a queued stop cancels a queued continue.

SIGKILL also receives special handling: if the target is stopped (`PROC_STOPPED`), SIGKILL forces it back to `PROC_READY` so the kill can be delivered. No other signal can wake a stopped process — only SIGCONT and SIGKILL.

From the keyboard driver, Ctrl+C and Ctrl+Z now flow through the TTY layer rather than through a foreground heuristic. `tty_ctrl_c(0)` sends `SIGINT` to `tty0`'s foreground process group; `tty_ctrl_z(0)` sends `SIGTSTP` to that same group. While the shell owns the foreground TTY, those signals are delivered to the shell itself and its prompt-time handlers redraw the prompt. While a job owns the foreground TTY, the entire foreground process group receives the signal, including both sides of a pipeline.

User programs send signals through `SYS_KILL` by passing either a positive PID or a negative process-group ID and a signal number. The call returns zero on success and −1 if the signal number is out of range.

`SYS_EXIT_GROUP` terminates the current thread group. The scheduler marks the group as exiting, wakes group waiters, and causes every live task in that group to observe the group-exit state before it returns to user mode. Plain `SYS_EXIT` exits only the calling task; the group becomes waitable after the last task leaves.

### The Delivery Window

The kernel faces a dangerous moment: a signal can arrive while the process is mid-instruction. We solve this by deferring delivery until one specific safe point — right before returning to user space.

A signal's pending bit is set the moment `sched_send_signal` runs — possibly during an interrupt, possibly during another process's syscall. The signal is not delivered at that instant. Delivery happens at one specific location: **the moment a process is about to return to ring-3 code after a syscall or IRQ**.

The life of a signal, from send to handler return:

![](diagrams/ch19-diag01.svg)

This is the only safe point for delivery because it is the only moment when the full user-space register context is sitting in a complete, well-defined frame on the kernel stack and can be copied and redirected without corrupting anything.

Both the syscall return path and the IRQ return path call the signal check routine immediately before their final restore-and-return sequence. They pass the current stack pointer as an argument; the routine returns the stack pointer to use for the restore — unchanged if no signal is delivered, or adjusted to point at a newly built signal frame if one was pushed onto the user stack. The delivery mechanism is therefore invisible to the rest of the return path — it sees only "here is the frame to restore."

The signal check routine first checks task-directed pending bits and then the thread group's process-directed pending bits, always applying the current task's `sig_blocked` mask. Task-directed signals are checked first because they name a specific task by TID — deferring them in favour of process-directed signals that any task in the group could handle would violate the caller's intent. It picks the lowest-numbered deliverable signal, clears the pending bit from its source, and looks up the handler:

- **SIG_IGN**: nothing to do; returns unchanged.
- **SIG_DFL** and fatal: marks the process as signal-killed and calls the scheduler to switch immediately to the next ready process. The return path eventually restores some other process's register frame and returns to it.
- **User handler**: pushes a signal frame on the user stack and redirects the saved return address to the handler. The return path restores the (now-modified) frame and jumps to the handler instead of the original user code.

A check on the privilege level of the saved frame ensures signals are never delivered when returning to kernel-mode code. Signals are strictly a user-space concept.

One subtlety remains when a process is asleep inside a blocking syscall. A signal wakeup moves the process from `PROC_BLOCKED` back to `PROC_READY`, but the resumed context is still inside the kernel loop that was implementing the syscall. Pipe reads, pipe writes, and TTY reads therefore check `cur->sig_pending` after each wakeup and return early with an interrupt-style error if a signal is pending. Control then reaches the syscall return path, which calls the signal check routine with the syscall's user-mode return frame, and the signal is delivered normally.

### Signal Delivery: The Arch-Specific Dance

The queueing and disposition logic described above is fully portable. What changes between architectures is the mechanism the kernel uses to forge a handler invocation on the user stack. The kernel must construct a frame on the user stack that looks, to the returning hardware, like a normal function call into the handler — with a return address that leads back to a small piece of trampoline code that will issue a `sigreturn` system call to recover the original execution context. The exact shape of that frame mirrors the architecture's own trap frame, because the registers that need to be saved and restored are architecture-specific.

#### On x86: signal trampoline and sigreturn

On x86 the kernel builds a **signal frame** — a 32-byte structure pushed onto the user stack — when a user handler must be called. The frame saves everything needed to resume normal execution after the handler returns: the original instruction pointer (EIP), the flags register (EFLAGS), the general-purpose register that holds the syscall return value (EAX), and the original user stack pointer (ESP). It also embeds a short trampoline sequence directly in the frame — two instructions that move the `SYS_SIGRETURN` number into `EAX` and execute `int 0x80`.

After the frame is built, the kernel modifies the saved i386 trap frame so that `iret` jumps to the handler with the signal number as its argument and `[esp]` pointing at the trampoline's address. To the handler, this looks exactly like an ordinary C function call.

![](diagrams/ch19-diag02.svg)

When the handler executes `ret`, the CPU pops the trampoline address and executes the two-instruction sequence, which re-enters the kernel via `int 0x80` as `SYS_SIGRETURN`. The sigreturn handler reads the saved context back out of the signal frame and writes it into the current trap frame — restoring the original EIP, EFLAGS, EAX, and ESP. When `iret` runs, the process resumes exactly where it was interrupted, with registers in the state they had before the signal arrived.

#### On AArch64: signal trampoline and sigreturn

*On AArch64 (planned, milestone 4): the signal trampoline forges an AArch64 trap frame on the user stack and the trampoline's `svc #0` to `sigreturn` restores it.* Note this is structurally the same shape as on x86, just over the AArch64 register set.

Where x86 saves EIP, EFLAGS, EAX, and ESP, the AArch64 frame saves `ELR_EL1` (the interrupted instruction address), `SPSR_EL1` (the saved processor state), `SP_EL0` (the user stack pointer), and enough of the general-purpose register file to reconstruct the pre-signal state. The embedded trampoline issues `svc #0` rather than `int 0x80`. The kernel's `sigreturn` handler on AArch64 reads those fields back and writes them into the current EL1 exception frame so that `eret` returns the process to where it was before the signal. The user-visible contract — handler called as a C function, `sigreturn` called on exit, original context fully restored — is identical to x86.

### Registering and Masking Signals

A process changes the disposition of a signal through `SYS_SIGACTION`. Passing `SIG_DFL` (zero) restores the platform default; passing `SIG_IGN` (one) silently discards the signal. Any other value is treated as the virtual address of a handler function with signature `void handler(int signum)`. The call also accepts an optional output pointer where the previous disposition is written before the new one is installed, which lets library code chain handlers. SIGKILL and SIGSTOP have fixed dispositions — the kernel refuses to change them.

The signal mask is modified through `SYS_SIGPROCMASK`. The caller specifies both a mode and a bitmask. Mode `SIG_BLOCK` ORs the supplied bits into the current mask, adding signals to the blocked set; `SIG_UNBLOCK` clears those bits; `SIG_SETMASK` replaces the mask entirely. Regardless of mode, the kernel always strips bits 9 (SIGKILL) and 19 (SIGSTOP) from the resulting mask — those two signals cannot be blocked by any process, ever.

A signal that is both pending and blocked is held indefinitely in `sig_pending`. It will be delivered the first time the process's `sig_blocked` mask is cleared at that bit, either by an explicit `SYS_SIGPROCMASK` call or by returning from a signal handler that temporarily blocked other signals.

### Stop and Continue (Job Control Signals)

Three signals form the stop/continue mechanism that enables job control in the shell:

**SIGSTOP (19)** is the uncatchable stop. Like SIGKILL, it cannot be caught, blocked, or ignored. When the signal check routine encounters a pending SIGSTOP, it transitions the process to `PROC_STOPPED`, encodes the stop signal in `exit_status` using Linux-compatible encoding `(stop_signal << 8) | 0x7F`, sends SIGCHLD to the parent, and wakes the parent if it is waiting. The scheduler then switches away. The stopped process will not be picked by the scheduler until it receives SIGCONT.

**SIGTSTP (20)** is the terminal stop — sent to the foreground process group when the user presses Ctrl+Z. Unlike SIGSTOP, SIGTSTP *can* be caught or ignored. If the handler is SIG_DFL, the process is stopped exactly as with SIGSTOP. If the process has installed a handler, the signal is delivered as a normal signal frame, and the handler can choose to do cleanup before stopping (or to ignore the stop entirely). If the disposition is SIG_IGN, the signal is discarded.

**SIGCONT (18)** resumes a stopped process. The wake-up happens inside `sched_send_signal` itself — the target is transitioned from `PROC_STOPPED` back to `PROC_READY` immediately, before any signal-check pass. If the process has installed a SIGCONT handler, the handler is delivered the next time the process returns to user space. Otherwise the signal is silently discarded after the wake-up.

The Ctrl+Z delivery chain mirrors the Ctrl+C chain. The keyboard IRQ handler detects scancode 0x2C with Ctrl held, and calls `tty_ctrl_z(0)`. The TTY driver prints `^Z\n` and, if `fg_pgid` is set, sends SIGTSTP to the foreground process group via `sched_send_signal_to_pgid`.

### Wait Status Encoding

When a parent waits for a child via `SYS_WAIT` or `SYS_WAITPID`, the kernel returns a Linux-compatible encoded status word rather than the raw exit code. This encoding lets the caller distinguish between a child that exited normally and one that was stopped by a signal:

| Condition | Encoding | Macro |
|-----------|----------|-------|
| Normal exit (code N) | `(N << 8) \| 0x00` | `WIFEXITED(s)` true, `WEXITSTATUS(s)` = N |
| Stopped by signal S | `(S << 8) \| 0x7F` | `WIFSTOPPED(s)` true, `WSTOPSIG(s)` = S |

`SYS_WAITPID (188)` extends `SYS_WAIT` with option flags:

- **WUNTRACED**: also return when a child enters `PROC_STOPPED` (needed for the shell to detect Ctrl+Z).
- **WNOHANG**: return 0 immediately if no child has changed state (used by `jobs` to check background processes without blocking).

The shell uses `sys_waitpid(pid, WUNTRACED)` when running a foreground program. If the child is stopped, the shell adds it to its job list and re-prompts. The `fg` builtin later calls `sys_kill(pid, SIGCONT)` and `sys_waitpid(pid, WUNTRACED)` to resume and re-wait.

### Terminal Ownership

Two syscalls manage which process group owns the terminal. `SYS_TCSETPGRP` sets the TTY's `fg_pgid` to a given process group ID; the shell calls it before launching a foreground child to hand the terminal over, and calls it again after the child exits or stops to reclaim it. `SYS_TCGETPGRP` reads the current `fg_pgid` back — useful when a process wants to check whether it is still in the foreground before performing terminal I/O.

### Where the Machine Is by the End of Chapter 19

Every process now carries a 32-entry signal disposition table and two bitmasks. The keyboard driver sends SIGINT on Ctrl+C and SIGTSTP on Ctrl+Z. Signals are held as pending bits until the target process is about to return to user space, at which point the signal check routine delivers them — terminating the process (SIG_DFL for fatal signals), stopping it (SIGSTOP/SIGTSTP), or constructing a signal frame on the user stack to redirect execution to an installed handler.

The signal frame and trampoline mechanism is the arch-specific core of delivery. On x86, the frame preserves the i386 register context and the embedded trampoline issues `int 0x80` to call `SYS_SIGRETURN`. On AArch64, the same structural approach applies over the AArch64 register set, with the trampoline issuing `svc #0` instead. Both architectures share the same portable queueing, disposition, and stop/continue logic.

A stopped process enters `PROC_STOPPED` and is invisible to the scheduler until SIGCONT (or SIGKILL) arrives. The shell detects this via `SYS_WAITPID` with `WUNTRACED`, adds the stopped child to its job list, and offers `fg`, `bg`, and `jobs` builtins for resuming or inspecting jobs.

The signal path now covers the full lifecycle: asynchronous delivery, handler invocation and return via a per-arch trampoline, process stop and continue, and wait-status reporting that distinguishes exit from stop. Together with the TTY's foreground process group tracking (Chapter 18), this gives us a complete job control subsystem.
