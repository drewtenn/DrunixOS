.section .rodata

.global arm64_smoke_elf_start
.global arm64_smoke_elf_end

.balign 16
arm64_smoke_elf_start:
    .incbin "build/arm64-smoke-user.elf"
arm64_smoke_elf_end:
