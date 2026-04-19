# Code Standards Cleanup Audit Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement accepted cleanup tasks. This document is a triage plan; do not modify code until each item is accepted.

**Goal:** Build a reviewable cleanup backlog from recognized C/kernel coding standards, then decide item by item which changes are worth making.

**Architecture:** Use Linux kernel coding style as the primary local style baseline because Drunix is a C kernel. Use SEI CERT C as a reliability and security overlay for memory, string, integer, error-handling, and API-boundary risks.

**Tech Stack:** Freestanding C kernel, userland C compatibility tests, GNU Make, KTEST, QEMU checks, existing Drunix helpers.

---

## Standards Baseline

- Linux kernel coding style:
  - Drunix exception: use tabs for C block indentation, configured as 4 columns.
  - Prefer 80-column lines, with exceptions where breaking harms readability.
  - Function opening braces on the next line; non-function block braces on the same line.
  - Avoid multiple statements on one line.
  - Keep functions short and focused; split complex functions.
  - Use centralized cleanup exits when a function has shared cleanup.
  - Prefer comments that explain what/why, not obvious mechanics.

- SEI CERT C:
  - Treat security, reliability, portability, and undefined behavior as cleanup concerns, not only formatting.
  - Pay special attention to arrays, strings, memory management, input/output, error handling, APIs, concurrency, and undefined behavior.

## Initial Findings

### 1. Establish A Local Drunix C Style Policy

**Why:** The repo has documentation style rules but no C style policy. This makes cleanup subjective.

**Evidence:**
- `CLAUDE.md` points to repo docs policy files, but none define C coding style.
- Formatting differs between files. For example, `kernel/drivers/ata.c` uses four-space indentation and same-line function braces, while much of `kernel/proc/syscall.c` uses Linux-style function braces.

**Policy applied:** Add `docs/contributing/c-style.md` and link it from `CLAUDE.md`. Adopt "Linux kernel style with Drunix-specific exceptions" rather than trying to reformat everything at once. The first explicit Drunix exception is that tabs are still required for indentation, but they are configured as 4 columns instead of the Linux kernel's 8 columns.

### 2. Normalize Formatting Only In Touched Files

**Why:** Whole-repo formatting churn would hide behavioral changes. Formatting should be paid down in local, reviewable slices.

**Evidence:**
- `kernel/drivers/ata.c:18`, `kernel/drivers/ata.c:25`, `kernel/drivers/ata.c:33`, and nearby functions use same-line function braces.
- `kernel/drivers/ata.c:48`, `kernel/drivers/ata.c:49`, `kernel/drivers/ata.c:107`, `kernel/drivers/ata.c:113`, `kernel/drivers/ata.c:120`, `kernel/drivers/ata.c:126`, and `kernel/drivers/ata.c:132` use one-line conditionals.
- `kernel/drivers/ata.c` uses four-space indentation throughout, which conflicts with Linux kernel indentation guidance.

**Policy applied:** When a file is touched for behavior, normalize the entire file to the local C style in the same change set, while keeping behavior changes easy to identify. For pure cleanup, handle one file per commit.

### 3. Split Very Large Files And Functions Carefully

**Why:** Large files and long functions make review, testing, and bug isolation harder. Linux style explicitly recommends short, focused functions, with limited exceptions for simple case dispatchers.

**Evidence:**
- `kernel/proc/syscall.c` has 5,945 lines.
- `kernel/gui/desktop.c` has 2,909 lines.
- `kernel/fs/ext3.c` has 2,868 lines.
- `user/shell.c` has 2,729 lines.
- `syscall_handler` is about 325 lines at `kernel/proc/syscall.c:5628`; it is a simple dispatch table, so it may be acceptable, but the whole file is still hard to navigate.
- Complex functions above 100 lines include `kernel/gui/desktop.c:1776`, `kernel/proc/syscall.c:1338`, `kernel/fs/ext3.c:2611`, `kernel/proc/syscall.c:2690`, `kernel/proc/syscall.c:1197`, `kernel/fs/ext3.c:1482`, `kernel/fs/ext3.c:1163`, and `kernel/proc/syscall.c:2240`.

**Candidate decision:** Do not split files immediately. First identify natural module boundaries:
- syscall metadata/translation helpers
- syscall fd helpers
- syscall path helpers
- ext3 block/inode helpers
- ext3 directory operations
- ext3 journal replay
- desktop rendering vs input/window management

### 4. Centralize Cleanup Paths Where Allocations Repeat

**Why:** Repeated `kmalloc`/`kfree` chains create easy leak and double-free mistakes. Linux style explicitly supports named `goto` exits for shared cleanup.

**Evidence:**
- `kernel/proc/syscall.c` has repeated 4096-byte path allocation and free paths around `resolve_user_path` and `resolve_user_path_at`.
- `kernel/fs/ext3.c:1220` allocates `link_path`, `target`, and `next`, then manually frees all three on multiple exits.
- Similar symlink-resolution allocation patterns appear in `kernel/fs/fs.c:571`, `kernel/fs/fs.c:821`, `kernel/fs/fs.c:1200`, and `kernel/fs/fs.c:1406`.

**Candidate decision:** Prefer small helpers or named cleanup labels for repeated allocation groups. Keep return codes identical.

### 5. Replace Repeated Literal Capacities With Named Constants

**Why:** Repeated buffer sizes like `4096` obscure whether the value means page size, path capacity, filesystem block size, or syscall scratch space.

**Evidence:**
- `kernel/proc/syscall.c` repeatedly allocates `4096`-byte path buffers.
- `kernel/fs/ext3.c:1220` through `kernel/fs/ext3.c:1248` uses `4096` as a symlink path scratch capacity.
- `kernel/fs/fs.c` has several `4096` symlink path scratch allocations despite `DUFS_BLOCK_SIZE` already existing for filesystem blocks.
- `kernel/mm/paging.c` has raw `0xFFFu`, `0x1000u`, `0x3FFu`, and `1024` values in paging math where some constants already exist in headers.

**Candidate decision:** Introduce narrow names such as `SYSCALL_PATH_MAX`, `EXT3_PATH_SCRATCH_SIZE`, or use existing `PAGE_SIZE` only when the meaning truly is a page.

### 6. Standardize Error Conventions Per Layer

**Why:** Mixed `-1`, `(uint32_t)-1`, Linux negative errno, and subsystem-specific negative values make cleanup risky.

**Evidence:**
- `kernel/proc/syscall.c` intentionally mixes generic Drunix failure returns and Linux ABI negative errno returns.
- `kernel/fs/ext3.c:1176` returns `-40` for symlink loop depth, likely meaning `ELOOP`, but most local filesystem functions otherwise return `-1`.
- `kernel/drivers/mouse.c` returns `-11` and `-12` in a small number of cases while most driver failures return `-1`.

**Candidate decision:** Document conventions first. Then convert only call paths where tests already assert the user-visible errno behavior.

### 7. Reduce Single-Line Control Flow In Kernel Code

**Why:** Single-line `if (...) return ...;` is compact but becomes error-prone when a condition gains logging or cleanup. The codebase uses both styles.

**Evidence:**
- `kernel/drivers/ata.c:48`, `kernel/drivers/ata.c:49`, `kernel/drivers/ata.c:107`, `kernel/drivers/ata.c:113`, `kernel/drivers/ata.c:120`, `kernel/drivers/ata.c:126`, and `kernel/drivers/ata.c:132`.
- `kernel/module.c:93`, `kernel/module.c:99`, `kernel/module.c:100`, `kernel/module.c:101`, and `kernel/module.c:102`.
- `kernel/blk/bcache.c:108`, `kernel/blk/bcache.c:197`, `kernel/blk/bcache.c:208`, `kernel/blk/bcache.c:228`, and related lines.

**Candidate decision:** Prefer multi-line form in kernel code, especially when a function already has cleanup or logging.

### 8. Add Mechanical Style Checks

**Why:** Manual review will regress. The repo currently allows style drift.

**Evidence:**
- No `.editorconfig` file was found.
- No dedicated C style check script was found.
- Long-line scan finds many real long lines, plus legitimate table/generated exceptions such as `kernel/gui/font8x16.c`.
- Trailing whitespace scan across `kernel` and `user` C files found no matches, which is good.

**Candidate decision:** Add a lightweight `tools/check_c_style.py` or Make target that reports, but initially does not fail, on:
- trailing whitespace
- long lines outside allowlisted generated/table files
- same-line function opening braces
- one-line conditionals in `kernel/`

## Proposed Review Order

1. Agree on the standards baseline and repo-specific exceptions.
2. Add a written Drunix C style policy.
3. Add non-failing style audit tooling.
4. Pick one low-risk file for pure formatting cleanup, likely `kernel/drivers/ata.c`.
5. Address allocation cleanup patterns in the existing syscall/VFS/DUFS cleanup plan.
6. Decide whether to split large files or only extract helpers inside them.
7. Standardize error-return documentation before changing any return values.

## Verification For Any Accepted Cleanup

- Run `git diff --check`.
- Run `make kernel`.
- Run `make check`.
- For syscall or Linux ABI changes, run the Linux ABI compatibility target or harness used by `user/linuxabi.c`.
- For filesystem changes, run focused VFS/FS tests plus the full suite.

## Non-Goals

- No whole-repo auto-format pass.
- No behavior changes hidden inside style commits.
- No return-code conversions until the intended API contract is written down.
- No generated/font/table reflow unless that file is specifically being maintained.
