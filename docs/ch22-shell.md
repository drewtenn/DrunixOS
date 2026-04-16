\newpage

## Chapter 22 — The Interactive Shell

### What the Shell Is in the Current System

Chapter 21 left us with a complete C runtime library and every user program able to call `printf`, `malloc`, and `fopen` without touching assembly. The shell is an ordinary ring-3 process stored on the filesystem as `bin/shell`. The kernel starts exactly one instance during boot: it looks up `bin/shell` in DUFS, creates a process for it with an initial environment of `PATH=/bin`, and schedules that process first.

After that, the shell stays alive as a parent process. It does not get replaced by child programs and is not automatically relaunched by the kernel after every command exits. Instead it follows the classic read-eval-print loop that every Unix shell has used since 1971: print a prompt, read a command line, execute the command, and repeat. If the shell itself exits and no other process is runnable, the scheduler halts the machine.

### The Read-Eval-Print Loop

By the time `_start` hands control to `main`, the kernel has already placed the user at a shell prompt. From that moment on, everything the user sees is driven by one tight loop.

`readline` blocks inside a `SYS_READ` call, waiting for the keyboard driver to produce a character. When a character arrives it is echoed immediately and appended to the line buffer. A backspace removes the last character from the buffer and overwrites it on screen with a space. Page Up and Page Down invoke the same scrollback syscalls in both display modes: VGA fallback scrolls the legacy console history, while framebuffer desktop mode scrolls the shell terminal surface and updates its pixel scrollbar. `ETX` (End-of-Text, the byte value `0x03` produced when the user holds Ctrl and presses C) discards the current buffer and re-displays the prompt. When Enter arrives the buffer is returned to the caller as a complete line.

The returned line is tokenised in place. The shell scans for whitespace, replaces each run of it with a null byte, and fills a `char *tokens[]` array with pointers into the original buffer. No heap allocation is involved: each pointer directly addresses a substring of the original line, which is already a valid C string because the null replacement terminates it. The first token is the command name; the rest are its arguments.

The command name is checked against a table of built-in commands. Built-ins run directly inside the shell process — they can modify the shell's own environment table and working directory in ways that an external child process cannot. Any name that is not a built-in reaches the external program launcher, which is described later in this chapter.

### Current Working Directory

Every process in the kernel carries a working directory string inside its descriptor, initialised to the root `/`. Two syscalls manage it: one validates a target path and, if it names an existing directory, writes the new path into the process descriptor; the other copies the current path into a user-supplied buffer. Because the working directory lives in the kernel descriptor rather than in user space, it is automatically inherited when a process forks — the child's descriptor is a copy of the parent's, working directory included.

The shell's `cd` built-in calls the first syscall directly. The prompt-generation code calls the second before each prompt to display the current directory. The kernel-side implementation handles `NULL`, empty strings, and `"/"` (go to root), `".."` (strip the last path component), absolute paths (beginning with `/`), and paths relative to the current directory — all resolved inside the kernel without requiring the shell to perform any path arithmetic itself.

### Environment Variables

The shell maintains a private variable table — a fixed-size array where each entry stores a name, a value, an exported flag, and a read-only flag. At startup, it walks the `envp` (environment pointer) array that the startup stub receives from the kernel and populates the table from any `NAME=VALUE` strings already present. The initial environment the kernel provides is `PATH=/bin`.

The exported flag determines whether a variable is passed to child processes. Before launching any child program the shell walks the table and constructs a flat null-terminated `char *envp[]` array from all exported entries. A forked child inherits that vector through the global `environ` pointer, and a subsequent `SYS_EXEC` uses it to build the replacement image's initial environment frame. Because the kernel lays the environment strings out on the new user stack alongside the argument strings (Chapter 15), the program sees a fully populated environment the moment its `_start` runs.

The read-only flag causes the shell to reject subsequent assignments to a variable. Shell-local variables exist in the table but are omitted from the exported `envp[]` rebuild, so they are invisible to children.

### PATH-Based Command Resolution

When the user types a name that does not match any built-in, the shell searches for a corresponding **ELF** (Executable and Linkable Format, the binary format described in Chapter 15) file on disk. The search follows the standard `PATH` algorithm.

If the name contains a `/`, it is treated as a literal path and probed directly. Otherwise, the shell splits the `PATH` variable on `:` to obtain a list of directories and, for each directory, concatenates the directory path with the command name and probes whether the result exists. The first match wins.

Because the kernel initialises `PATH=/bin`, all programs installed in `/bin` on the DUFS image are reachable by name without qualification. `PATH` is a regular environment variable, so a user can extend it with additional directories the same way any other variable is set.

### Running an External Program

Launching an external program requires four kernel operations in sequence.

First, `SYS_FORK` clones the shell. The child resets its foreground-job signal handlers to defaults so Ctrl+C and Ctrl+Z affect the program it is about to run rather than bouncing back into shell prompt logic.

Second, the child calls `SYS_EXEC` with the path, argument vector, and exported environment. The kernel loads the **ELF** binary from disk, allocates a fresh page directory for it, maps its segments into user space, rebuilds the initial user stack, and replaces the child process image in place. The child's PID does not change.

Third, the shell calls `SYS_SETPGID` to place the child in its own **process group** — a collection of processes that share a common group identifier, the **PGID** (Process Group ID). This is the foundation of job control: signals sent to a PGID reach every process in the group simultaneously, and the TTY driver directs keyboard input only to the group that currently holds the terminal foreground.

Fourth, the shell calls `SYS_TCSETPGRP` to hand the terminal to the child's process group. The TTY's foreground PGID is updated to the child's; from this moment Ctrl+C and Ctrl+Z signals are directed at the child rather than the shell. The shell then calls `SYS_WAITPID` and blocks until the child either exits or stops.

When the child terminates, the shell reclaims the terminal by calling `SYS_TCSETPGRP` with its own PGID before printing the next prompt.

### Pipe Support

The shell supports a single `|` operator connecting two external programs. After tokenising the line the shell scans for a `|` token; when found, everything to the left is the writer command and everything to the right is the reader command.

To connect them, the shell first asks the kernel to allocate a **pipe** — a unidirectional in-kernel ring buffer with two **fd** (file descriptor) handles: a read end and a write end. It then forks twice.

The left child replaces its standard output (fd 1) with the write end of the pipe using `SYS_DUP2`, closes the raw pipe fds, and execs the left command. The right child replaces its standard input (fd 0) with the read end using `SYS_DUP2`, closes its pipe fds, and execs the right command. The parent closes both pipe ends.

Closing the write end in the parent is the critical detail. When the left child exits, both the child's copy and the parent's copy of the write end are closed. The right child then sees **EOF** (End of File — the condition that no further data will ever arrive) on its read end and terminates naturally, rather than blocking forever waiting for input that can never come.

Both children run in the same process group so a Ctrl+Z stops the entire pipeline together. `SYS_EXEC` preserves the calling process's fd table, so the `dup2` redirection performed in the forked child before exec is transparently visible to the exec'd program.

### Job Control

The shell maintains a job table of up to eight entries. Each entry records a child PID, its stopped or running state, and the original command name. The built-in job-management commands (`jobs`, `fg`, `bg`) operate on this table.

When the shell starts, it establishes itself as its own process group leader, claims the foreground TTY for that PGID, and installs prompt-time handlers for `SIGINT` and `SIGTSTP`. That means Ctrl+C and Ctrl+Z are real signals even at the prompt; the shell's handlers simply abandon the current input line and redisplay the prompt instead of terminating or stopping the shell itself.

When the shell launches a foreground child and calls `SYS_TCSETPGRP`, the TTY's foreground PGID changes to the child's. Now Ctrl+C sends `SIGINT` to the entire child process group and Ctrl+Z sends `SIGTSTP`. If the child stops due to `SIGTSTP`, `SYS_WAITPID` returns with a stopped status. The shell detects this — the status word encodes whether the child exited or was merely stopped — adds the child to the job table, reclaims the terminal for its own PGID, and re-prompts. The stopped child can be resumed in the foreground with `fg` or continued in the background with `bg`.

### Prompt and ANSI Colours

Before printing each prompt the shell queries the kernel for the current working directory and assembles the prompt string using **ANSI** (American National Standards Institute) terminal escape sequences. An ANSI escape sequence is a byte string beginning with the ESC character (`0x1B`) followed by `[`, a numeric attribute code, and `m`. The active console presentation path interprets these sequences, applying the requested colour to subsequent characters until a reset sequence is encountered. In the framebuffer desktop this happens in the shell terminal surface before its glyphs are rendered as pixels; in VGA fallback mode it happens in the legacy text console.

The shell prints the hostname in cyan, the working directory in green, and the `>` separator in cyan, then resets the colour before the cursor — keeping the user's typed input in the default white-on-black style rather than inheriting the prompt colour.

### Tab Completion

When the user presses Tab, the shell treats it as a completion request rather than echoing the character. The completer examines the buffer to find the word being completed — the contiguous run of non-space characters that ends at the cursor — and dispatches to one of two strategies.

If the word is the first token on the line (no space has been typed yet), the completer performs **command completion**: it searches a table of all built-in names, then walks each directory in `$PATH` and enumerates its files via `SYS_GETDENTS`, collecting every entry whose name starts with the typed prefix.

If at least one space precedes the word, the completer performs **path completion**: the word is split at its last `/` to obtain a directory component and a name prefix. `SYS_GETDENTS` is called on that directory (or on the process's working directory when no `/` is present), and every entry that starts with the name prefix is collected as a candidate. Directory entries are returned by the kernel with a trailing `/`, which is preserved in the match so the user can see immediately whether a completion names a file or a subdirectory.

Once the match list is assembled, the completer picks a course of action based on how many candidates were found:

| Matches | Action | Result |
|---------|--------|--------|
| `0` | Ring bell | There is nothing to complete |
| `1` | Insert suffix | The word is completed immediately |
| `2+` | Longest prefix or list | The word extends if possible, or choices are shown |

The longest-common-prefix step mirrors the behaviour of every POSIX shell: the first Tab extends as far as possible without guessing, and the second Tab (or a Tab when no extension is possible) reveals the ambiguous options.

All of this happens entirely inside the shell's line editor — no new syscalls are involved. The completer writes directly to standard output using the same output path the rest of the line editor uses for echo and backspace, keeping the screen state consistent with the buffer at every step.

### Running Inside the Desktop

The boot shell is still an ordinary ring-3 process, but the kernel now opens it as one window inside the desktop rather than as the only desktop surface. The launcher can open built-in Files, Processes, and Help windows. Standard output from the shell, its direct children, and the TTY foreground process group continues to route to the shell terminal window even while another built-in mini app has focus. That routing matters because the shell may be visually behind a Files or Processes window while a foreground program is still writing to fd 1. The desktop checks process ownership before accepting console bytes; output that does not belong to the shell session falls back to the legacy console path instead of being mixed into the shell surface. If the framebuffer path is available, the shell terminal surface renders its padded `8x16` glyph grid, underline cursor, and scrollback view directly into the linear framebuffer; otherwise the same terminal state is mirrored into cells for the VGA text fallback.

The mouse pointer is desktop state rather than shell state. PS/2 mouse packets move a pixel-positioned cursor, and clicking a window focuses it and raises it above its siblings. Clicking a title bar starts a drag, clicking a close button closes that window, and clicking a taskbar entry focuses the matching open window. The taskbar's menu region toggles the launcher, whose entries can open the shell, Files, Processes, or Help. When the Processes window receives focus, its contents are refreshed from the current process table so the view is a snapshot of the system at the moment the user asks to see it. The shell does not need to know any of this happened: it continues to read fd 0, write fd 1, and manage jobs through the same syscalls as before.

### Where the Machine Is by the End of Chapter 22

We now have a real interactive parent process with job control, environment management, and PATH-based command resolution. The shell can navigate an arbitrary directory tree, launch external programs with argument vectors and an inherited environment, connect two programs through a kernel pipe, and suspend and resume jobs with terminal handoff. Every user-visible action goes through the syscall layer, making the shell the place where the process, filesystem, driver, environment, console, and desktop subsystems all meet.
