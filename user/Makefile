PROGS = hello shell writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg

all: $(PROGS)

CC     = x86_64-elf-gcc
LD     = x86_64-elf-ld
NASM   = nasm
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
         -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
         -fdebug-prefix-map=$(abspath ..)=.

LIB_OBJS = lib/crt0.o lib/syscall.o lib/malloc.o \
           lib/string.o lib/ctype.o lib/stdlib.o lib/stdio.o \
           lib/unistd.o lib/time.o

lib/crt0.o: lib/crt0.asm
	$(NASM) -f elf32 -o $@ $<

lib/syscall.o: lib/syscall.c lib/syscall.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/malloc.o: lib/malloc.c lib/malloc.h lib/syscall.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/string.o: lib/string.c lib/string.h lib/malloc.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/ctype.o: lib/ctype.c lib/ctype.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/stdlib.o: lib/stdlib.c lib/stdlib.h lib/malloc.h lib/syscall.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/stdio.o: lib/stdio.c lib/stdio.h lib/string.h lib/stdlib.h lib/syscall.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/unistd.o: lib/unistd.c lib/unistd.h lib/syscall.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/time.o: lib/time.c lib/time.h lib/syscall.h lib/string.h
	$(CC) $(CFLAGS) -c -o $@ $<

# One pattern rule handles every user-program .o target.
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

hello: hello.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) hello.o

shell: shell.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) shell.o

writer: writer.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) writer.o

reader: reader.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) reader.o

sleeper: sleeper.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) sleeper.o

date: date.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) date.o

which: which.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) which.o

cat: cat.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) cat.o

echo: echo.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) echo.o

wc: wc.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) wc.o

grep: grep.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) grep.o

head: head.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) head.o

tail: tail.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) tail.o

tee: tee.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) tee.o

sleep: sleep.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) sleep.o

env: env.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) env.o

printenv: printenv.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) printenv.o

basename: basename.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) basename.o

dirname: dirname.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) dirname.o

cmp: cmp.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) cmp.o

yes: yes.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) yes.o

sort: sort.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) sort.o

uniq: uniq.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) uniq.o

cut: cut.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) cut.o

kill: kill.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) kill.o

crash: crash.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) crash.o

dmesg: dmesg.o $(LIB_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(LIB_OBJS) dmesg.o

clean:
	find . -name '*.o' -delete
	find . -name '*.elf' -delete
	$(RM) $(PROGS)
