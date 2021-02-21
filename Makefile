all: run

kernel.bin: kernel-entry.o kernel.o
	x86_64-elf-ld -m elf_i386 -o $@ -Ttext 0x1000 $^ --oformat binary

kernel.o: kernel.c
	x86_64-elf-gcc -m32 -g -ffreestanding -c $< -o $@

os-image.bin: boot_sect.bin kernel.bin
	cat $^ > $@

kernel.elf: kernel-entry.o kernel.o
	x86_64-elf-ld -m elf_i386 -o $@ -Ttext 0x1000 $^

debug: os-image.bin kernel.elf
	qemu-system-i386 -s -S -fda os-image.bin -d guest_errors,int &
	i386-elf-gdb -ex "target remote localhost:1234" -ex "symbol-file kernel.elf"

run: os-image.bin
	qemu-system-i386 -fda $<

%.o: %.asm
	nasm $< -f elf -o $@

%.bin: %.asm
	nasm $< -f bin -o $@

clean:
	$(RM) *.bin *.o *.dis