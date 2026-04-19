PROGS = chello hello shell writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk linuxhello linuxprobe linuxabi busybox tcc bbcompat dufstest redirtest ext3wtest threadtest tcccompat
C_PROGS = chello shell bbcompat dufstest redirtest ext3wtest threadtest tcccompat
CXX_PROGS = hello writer reader sleeper date which cat echo wc grep head tail tee sleep env printenv basename dirname cmp yes sort uniq cut kill crash dmesg cpphello lsblk
LINUX_PROGS = linuxhello linuxprobe linuxabi busybox tcc
LINUX_ASM_PROGS = linuxhello
LINUX_C_PROGS = linuxprobe linuxabi
LINUX_BUSYBOX_PROGS = busybox
LINUX_TCC_PROGS = tcc
CLEAN_PROGS = $(filter-out $(LINUX_BUSYBOX_PROGS) $(LINUX_TCC_PROGS),$(PROGS))

all: $(PROGS)

CC     = x86_64-elf-gcc
CXX    = x86_64-elf-g++
LD     = x86_64-elf-ld
AR     = x86_64-elf-ar
NASM   = nasm
LINUX_I386_CC ?= i486-linux-musl-gcc
BUSYBOX_VERSION ?= 1.36.1
BUSYBOX_BUILD_DIR ?= ../build/busybox
TCC_VERSION ?= 0.9.27
TCC_BUILD_DIR ?= ../build/tcc
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
         -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
         -fdebug-prefix-map=$(abspath ..)=.
CXXFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
           -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
           -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
           -fno-threadsafe-statics -fdebug-prefix-map=$(abspath ..)=.
LINUX_CFLAGS = -static -Os -s
CXXLIBS = $(shell $(CXX) -m32 -print-libgcc-file-name)

C_RUNTIME_OBJS = lib/crt0.o lib/cxx_init.o lib/syscall.o lib/malloc.o \
                 lib/string.o lib/ctype.o lib/stdlib.o lib/stdio.o \
                 lib/unistd.o lib/time.o
C_RUNTIME_LIB_OBJS = $(filter-out lib/crt0.o,$(C_RUNTIME_OBJS))
CXX_RUNTIME_OBJS = lib/cxxrt.o lib/cxxabi.o
C_LINK_OBJS = $(C_RUNTIME_OBJS)
CXX_LINK_OBJS = $(C_RUNTIME_OBJS) $(CXX_RUNTIME_OBJS)

lib/crt0.o: lib/crt0.asm
	$(NASM) -f elf32 -o $@ $<

lib/cxx_init.o: lib/cxx_init.c
	$(CC) $(CFLAGS) -c -o $@ $<

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

lib/libc.a: $(C_RUNTIME_LIB_OBJS)
	$(AR) rcs $@ $(C_RUNTIME_LIB_OBJS)

# One pattern rule handles every user-program .o target.
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lib/cxxrt.o: lib/cxxrt.cpp lib/malloc.h lib/syscall.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

lib/cxxabi.o: lib/cxxabi.cpp lib/syscall.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I . -c -o $@ $<

%.o: %.asm
	$(NASM) -f elf32 -o $@ $<

$(C_PROGS): %: %.o $(C_LINK_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(C_LINK_OBJS) $@.o

$(CXX_PROGS): %: %.o $(CXX_LINK_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(CXX_LINK_OBJS) $@.o $(CXXLIBS)

$(LINUX_ASM_PROGS): %: %.o
	$(LD) -m elf_i386 -nostdlib -e _start -Ttext 0x08048000 -o $@ $@.o

$(LINUX_C_PROGS): %: %.c
	$(LINUX_I386_CC) $(LINUX_CFLAGS) -o $@ $<

$(LINUX_BUSYBOX_PROGS):
	../tools/build_linux_busybox.sh $@ $(BUSYBOX_VERSION) $(BUSYBOX_BUILD_DIR)

$(LINUX_TCC_PROGS):
	../tools/build_linux_tcc.sh $@ $(TCC_VERSION) $(TCC_BUILD_DIR)

clean:
	find . -name '*.o' -delete
	find . -name '*.elf' -delete
	$(RM) lib/libc.a
	$(RM) $(CLEAN_PROGS)
