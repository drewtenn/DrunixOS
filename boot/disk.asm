; SPDX-License-Identifier: GPL-3.0-or-later
; boot/disk.asm
; Loads sectors from disk using INT 0x13 AH=42h (LBA Extended Read).
;
; Calling convention:
;   BX  = destination buffer offset (segment assumed 0x0000)
;   DH  = number of sectors to read
;   DL  = drive number (passed in from BIOS via BOOT_DRIVE)
;
; The BIOS places the boot drive number in DL before jumping to the MBR.
; USB drives are presented as hard disks (DL = 0x80+); floppies use DL = 0x00.
; boot_sect.asm saves DL to BOOT_DRIVE on its first instruction and restores
; it into DL before calling disk_load, so the correct device is always used.

disk_load:
    pusha

    ; Fill in the mutable DAP fields from the caller's registers
    mov byte  [dap_sectors], dh   ; number of sectors to read
    mov word  [dap_offset],  bx   ; destination buffer offset

    ; INT 0x13 AH=42h: Extended Read Sectors from Drive
    ;   DL = drive number (set by caller)
    ;   DS:SI = pointer to Disk Address Packet
    mov ah, 0x42
    mov si, dap
    int 0x13
    jc  disk_error   ; carry flag set = BIOS reported an error

    popa
    ret

disk_error:
disk_loop:
    jmp $

; ── Disk Address Packet (DAP) ─────────────────────────────────────────────────
; Layout (16 bytes, little-endian):
;   +0   db  0x10        packet size = 16 bytes (required by spec)
;   +1   db  0x00        reserved, must be zero
;   +2   dw  count       number of sectors to transfer  (filled at call time)
;   +4   dw  offset      destination buffer offset      (filled at call time)
;   +6   dw  segment     destination buffer segment     (fixed: 0x0000)
;   +8   dd  lba_lo      lower 32 bits of starting LBA  (fixed: LBA 1)
;   +12  dd  lba_hi      upper 32 bits of starting LBA  (fixed: 0)
;
; LBA 0 = MBR / boot sector (512 bytes, loaded by BIOS)
; LBA 1 = first byte of kernel binary (40 sectors = 20 KB)

dap:
dap_size:     db 0x10
dap_reserved: db 0x00
dap_sectors:  dw 0x0000     ; filled by disk_load
dap_offset:   dw 0x0000     ; filled by disk_load
dap_segment:  dw 0x0000     ; always 0x0000
dap_lba_lo:   dd 0x00000001 ; LBA 1 — sector immediately after the MBR
dap_lba_hi:   dd 0x00000000 ; upper 32 bits (zero for any reasonably-sized disk)
