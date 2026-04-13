\newpage

## Chapter 17 — File I/O and Open File Descriptors

### The File Descriptor Table

Chapter 16 left us with a complete syscall ABI and blocking wait queues. Every open file in a Unix-like system is identified by a small integer called a **file descriptor** (fd). The fd is not a pointer into the filesystem — it is an index into a per-process table. Each slot in the table holds our view of an open file: the inode number, the current read/write position, the file's cached size, and a flag indicating whether the descriptor is writable.

Each process carries this table inside its `process_t` descriptor. The table has a fixed number of slots. By convention, the first three are pre-reserved: fd 0 is standard input (connected to the keyboard character device), fd 1 is standard output (connected to the VGA console), and fd 2 is standard error (not yet implemented). Regular disk-backed files start at fd 3 and occupy higher slots.

When a process has two files open, the table might look like this:

| fd | Entry | Target | Position |
|----|-------|--------|----------|
| `0` | stdin device | Console input | Not seekable |
| `1` | stdout device | Console output | Not seekable |
| `2` | stderr device | Console errors | Not seekable |
| `3` | Open for reading | File A | Next read starts at byte 256 |
| `4` | Open for writing | File B | Next write starts at byte 0 |

Identifying files by inode number rather than by raw disk **LBA** (Logical Block Address) is what connects this layer to the inode-based filesystem in Chapter 13. The inode number is stable — it does not change when a file is renamed or when a directory entry is updated — so the open descriptor remains valid even if the file's name changes while it is open.

### Opening and Closing Files

`SYS_OPEN` resolves the caller's path string through the VFS (Virtual File System) layer, which returns an inode number and file size. The kernel then scans the current process's `open_files[]` array for the first free slot and installs that metadata with `offset = 0` and `writable = 0`. The returned value is the slot index — the fd number the caller uses for all subsequent operations on this file.

`SYS_CREATE` does the same but requests a writable slot. On the filesystem side, if a file with the given name already exists, it is truncated: its data blocks are freed and the inode size is reset to zero. If no such file exists, a fresh inode is allocated and a directory entry is created. Either way the descriptor starts at `offset = 0` and `size = 0`, representing an empty file ready to receive writes. DUFS stamps the file with the current wall-clock time as the creation or modification timestamp.

`SYS_CLOSE` marks the slot as free. If the descriptor was opened writable, we flush the inode before releasing the slot. In the current DUFS implementation, `fs_write` already persists the inode on every write, so the flush at close time is a no-op — but it exists as the correct hook for future caching layers.

### Reading

When a process calls `SYS_READ`, we dispatch based on the fd number. A read from fd 0 goes directly to standard input and blocks until terminal data is available. A read from fd 3 or higher goes through the file table.

For a file read, we first check whether the current `offset` has reached `size` — if so we return 0, signalling **EOF** (End of File). Otherwise the kernel asks the filesystem to read from that inode at the current offset into the caller's buffer. The filesystem layer performs the actual block-level I/O (Chapter 13), translating the logical byte offset into block indices and LBAs. On return, the descriptor's `offset` advances by the number of bytes read. Subsequent reads automatically continue from where the previous one stopped, because the offset is maintained across calls in the fd table entry.

`SYS_LSEEK` repositions the offset of an open file descriptor without reading or writing. It accepts a signed displacement and a mode — set to an absolute position (`SEEK_SET`), advance relative to the current position (`SEEK_CUR`), or move relative to the end of the file (`SEEK_END`). After a seek, the next read or write starts from the new position. Seeking is rejected on non-file descriptors such as pipes or the TTY.

### Creating and Writing

`SYS_FWRITE` writes through a writable descriptor. The kernel passes the descriptor's inode number, current offset, user buffer, and byte count down to the filesystem layer. There the write may allocate new data blocks on demand, perform read-modify-write for partial-block updates, advance through the inode's direct and indirect block chains, update the inode's size and modification timestamp, and flush the updated metadata to disk. On return, the descriptor's `offset` and cached `size` advance by the number of bytes written.

The write path is necessarily heavier than the read path because it may change the filesystem's physical layout — new blocks must be allocated, new indirect table entries may need to be written, and the bitmaps on disk must reflect the current allocation state. These writes happen synchronously: when `SYS_FWRITE` returns, the data is on disk.

### Deletion, Rename, and Directories

`SYS_UNLINK` removes a file by name. We resolve the path to an inode, decrement the inode's link count, and when the count reaches zero, free all data blocks (including any indirect and double-indirect chains) and clear the inode's bitmap bit. The directory entry is then cleared, making the name available for reuse.

`SYS_RENAME` moves a file or directory. Both the source and destination paths are resolved relative to the calling process's cwd. Moving a regular file between the root and a subdirectory is supported. Replacing an existing regular file at the destination is permitted. Replacing an existing directory is rejected.

`SYS_MKDIR` and `SYS_RMDIR` create and remove directories. Both paths are resolved relative to the cwd. `SYS_RMDIR` refuses to remove a directory that still contains entries.

`SYS_GETDENTS` enumerates the entries in a directory. When called with a path, it resolves to the corresponding directory inode and reads its directory blocks. When called with a null path and the process is at the root, it lists the root. Each returned entry carries the name, inode number, type, and size — enough for `ls` to display a complete directory listing without a separate `stat` call for each entry.

`SYS_STAT` fills a small metadata structure in user space with the file's type, byte size, link count, and last-modified timestamp. The timestamp is the Unix time in UTC seconds stored in the inode's `mtime` field.

### Pipes and the fd Table

`SYS_PIPE` also installs entries in the fd table. It allocates a shared ring buffer in kernel heap memory and creates two fd-table entries — one read-only, one write-only — both referencing the same buffer through a pointer rather than an inode number. Reads from the read end block when the buffer is empty; writes to the write end wake any blocked reader. When the last write-end descriptor is closed, the next read from the read end returns 0, signalling EOF to the reader.

### Where the Machine Is by the End of Chapter 17

The process descriptor now holds a live fd table that spans keyboard input (fd 0), console output (fd 1), and up to eight disk-backed or pipe-backed file descriptors starting at fd 3. The open, read, write, seek, and close operations all operate through inode numbers resolved at open time, so the filesystem layer and the syscall layer are cleanly separated: the syscall layer advances offsets and validates access flags; the filesystem layer handles the block allocation and disk I/O.
