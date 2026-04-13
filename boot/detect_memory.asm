; SPDX-License-Identifier: GPL-3.0-or-later
; detect_memory.asm
; Calls BIOS INT 0x15, EAX=0xE820 to build a physical memory map.
; Results are stored at 0x500:
;   [0x500]  dd  entry_count        (number of valid entries)
;   [0x504]  e820_entry_t[0..N-1]   (20 bytes each, up to 20 entries)
;
; e820_entry_t layout:
;   +0   dq  base_address
;   +8   dq  length
;   +16  dd  type  (1=usable, 2=reserved, 3=ACPI reclaimable, etc.)

[bits 16]

detect_memory:
    pusha
    xor  ax, ax
    mov  es, ax             ; ES = 0x0000 so ES:DI = linear address DI
    mov  di, 0x0504         ; first entry slot (count lives at 0x500)
    xor  ebx, ebx           ; EBX = 0: first BIOS call
    xor  bp, bp             ; BP = running entry count
    mov  edx, 0x534D4150    ; 'SMAP' magic

.next_entry:
    mov  eax, 0xE820
    mov  ecx, 20            ; request 20-byte entries
    int  0x15
    jc   .done              ; carry set = end of list or unsupported
    cmp  eax, 0x534D4150    ; BIOS must echo 'SMAP' back in EAX
    jne  .done

    ; Skip zero-length entries (some BIOSes emit them)
    mov  eax, [es:di + 8]   ; length low dword
    or   eax, [es:di + 12]  ; OR with length high dword
    jz   .skip

    inc  bp                 ; count this entry
    add  di, 20             ; advance write pointer
    cmp  bp, 20             ; hard cap at 20 entries
    je   .done

.skip:
    test ebx, ebx           ; EBX == 0 means this was the last entry
    jnz  .next_entry

.done:
    mov  dword [0x500], 0   ; zero-extend upper word first
    mov  [0x500], bp        ; store entry count as 32-bit value
    popa
    ret
