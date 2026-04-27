include programs.mk
CLEAN_PROGS = $(PROGS)

all: $(PROGS)

print-progs:
	@printf '%s\n' $(PROGS)

CC     = x86_64-elf-gcc
CXX    = x86_64-elf-g++
LD     = x86_64-elf-ld
AR     = x86_64-elf-ar
NASM   = nasm
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
         -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
         -Werror -fdebug-prefix-map=$(abspath ..)=.
CXXFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
           -fno-stack-protector -fno-omit-frame-pointer -g -Og -Wall \
           -Werror -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
           -fno-threadsafe-statics -fdebug-prefix-map=$(abspath ..)=.
NASMFLAGS = -Werror
CXXLIBS = $(shell $(CXX) -m32 -print-libgcc-file-name)

C_RUNTIME_OBJS = lib/crt0.o lib/cxx_init.o lib/syscall.o lib/malloc.o \
                 lib/string.o lib/ctype.o lib/stdlib.o lib/stdio.o \
                 lib/unistd.o lib/time.o
C_RUNTIME_LIB_OBJS = $(filter-out lib/crt0.o,$(C_RUNTIME_OBJS))
CXX_RUNTIME_OBJS = lib/cxxrt.o lib/cxxabi.o
C_LINK_OBJS = $(C_RUNTIME_OBJS)
CXX_LINK_OBJS = $(C_RUNTIME_OBJS) $(CXX_RUNTIME_OBJS)

lib/crt0.o: lib/crt0.asm
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

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
	$(CC) $(CFLAGS) -I../shared -c -o $@ $<

lib/cxxrt.o: lib/cxxrt.cpp lib/malloc.h lib/syscall.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

lib/cxxabi.o: lib/cxxabi.cpp lib/syscall.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I . -c -o $@ $<

%.o: %.asm
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(C_PROGS): %: %.o $(C_LINK_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(C_LINK_OBJS) $@.o

$(CXX_PROGS): %: %.o $(CXX_LINK_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(CXX_LINK_OBJS) $@.o $(CXXLIBS)

# desktop carries an embedded bitmap font and a vendored JPEG decoder
# in separate translation units; override the single-object C link
# rule to pull all four objects in.  nanojpeg.c stays unmodified
# under the original KeyJ Research License — the shim provides the
# allocator hooks the decoder calls when NJ_USE_LIBC is 0.
DESKTOP_NANOJPEG_OBJS = lib/nanojpeg/nanojpeg.o lib/nanojpeg/nanojpeg_shim.o

lib/nanojpeg/nanojpeg.o: lib/nanojpeg/nanojpeg.c lib/nanojpeg/nanojpeg.h
	$(CC) $(CFLAGS) -DNJ_USE_LIBC=0 '-DNULL=((void*)0)' -Wno-unused-function -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-parameter -Wno-implicit-fallthrough -c -o $@ $<

lib/nanojpeg/nanojpeg_shim.o: lib/nanojpeg/nanojpeg_shim.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The keymap source lives at the top-level shared/ directory so the
# kernel's VT keyboard handler and this user-space compositor build
# from the exact same translation logic.  We give the compiled object
# a unique path under the user tree so it doesn't collide with the
# kernel's build of the same source.
desktop_kbdmap.o: ../shared/kbdmap.c ../shared/kbdmap.h
	$(CC) $(CFLAGS) -I../shared -c -o $@ $<

desktop: desktop.o desktop_font.o desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS) $(C_LINK_OBJS) user.ld
	$(LD) -m elf_i386 -T user.ld -o $@ $(C_LINK_OBJS) desktop.o desktop_font.o desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS)

clean:
	find . -name '*.o' -delete
	find . -name '*.elf' -delete
	$(RM) lib/libc.a
	$(RM) $(CLEAN_PROGS)
