include programs.mk

USER_ARCH ?= x86
USER_LOAD_ADDR ?= 0x08000000
BUILD_ROOT ?= ../build/user/$(USER_ARCH)
BIN_DIR := $(BUILD_ROOT)/bin
OBJ_DIR := $(BUILD_ROOT)/obj
RUNTIME_DIR := $(BUILD_ROOT)/runtime
LINKER_DIR := $(BUILD_ROOT)/linker
APP_SRC_DIR := apps
RUNTIME_SRC_DIR := runtime
THIRD_PARTY_SRC_DIR := third_party
LINKER_SRC_DIR := linker

APP_OBJ_DIR := $(OBJ_DIR)/apps
RUNTIME_OBJ_DIR := $(OBJ_DIR)/runtime
THIRD_PARTY_OBJ_DIR := $(OBJ_DIR)/third_party
NANOJPEG_OBJ_DIR := $(THIRD_PARTY_OBJ_DIR)/nanojpeg

CLEAN_PROGS = $(addprefix $(BIN_DIR)/,$(PROGS))

all: $(CLEAN_PROGS) $(RUNTIME_DIR)/libc.a

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
USER_INCLUDES = -I $(RUNTIME_SRC_DIR) -I ../shared -I $(APP_SRC_DIR) -I $(THIRD_PARTY_SRC_DIR)/nanojpeg

C_RUNTIME_OBJS = $(RUNTIME_OBJ_DIR)/crt0.o $(RUNTIME_OBJ_DIR)/cxx_init.o $(RUNTIME_OBJ_DIR)/syscall.o $(RUNTIME_OBJ_DIR)/malloc.o \
                 $(RUNTIME_OBJ_DIR)/string.o $(RUNTIME_OBJ_DIR)/ctype.o $(RUNTIME_OBJ_DIR)/stdlib.o $(RUNTIME_OBJ_DIR)/stdio.o \
                 $(RUNTIME_OBJ_DIR)/unistd.o $(RUNTIME_OBJ_DIR)/time.o
C_RUNTIME_LIB_OBJS = $(filter-out $(RUNTIME_OBJ_DIR)/crt0.o,$(C_RUNTIME_OBJS))
CXX_RUNTIME_OBJS = $(RUNTIME_OBJ_DIR)/cxxrt.o $(RUNTIME_OBJ_DIR)/cxxabi.o
C_LINK_OBJS = $(C_RUNTIME_OBJS)
CXX_LINK_OBJS = $(C_RUNTIME_OBJS) $(CXX_RUNTIME_OBJS)

C_GENERIC_PROGS = $(filter-out desktop,$(C_PROGS))
C_BINS = $(addprefix $(BIN_DIR)/,$(C_GENERIC_PROGS))
CXX_BINS = $(addprefix $(BIN_DIR)/,$(CXX_PROGS))

$(LINKER_DIR)/user.ld: $(LINKER_SRC_DIR)/user.ld.in
	@mkdir -p $(dir $@)
	sed 's|@USER_LOAD_ADDR@|$(USER_LOAD_ADDR)|' $< > $@

$(RUNTIME_OBJ_DIR)/crt0.o: $(RUNTIME_SRC_DIR)/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: $(RUNTIME_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: $(RUNTIME_SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(USER_INCLUDES) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(USER_INCLUDES) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(RUNTIME_DIR)/libc.a: $(C_RUNTIME_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(C_RUNTIME_LIB_OBJS)

$(C_BINS): $(BIN_DIR)/%: $(APP_OBJ_DIR)/%.o $(C_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(C_LINK_OBJS) $(APP_OBJ_DIR)/$*.o

$(CXX_BINS): $(BIN_DIR)/%: $(APP_OBJ_DIR)/%.o $(CXX_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(CXX_LINK_OBJS) $(APP_OBJ_DIR)/$*.o $(CXXLIBS)

DESKTOP_NANOJPEG_OBJS = $(NANOJPEG_OBJ_DIR)/nanojpeg.o $(NANOJPEG_OBJ_DIR)/nanojpeg_shim.o

$(NANOJPEG_OBJ_DIR)/nanojpeg.o: $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg.c $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I $(RUNTIME_SRC_DIR) -DNJ_USE_LIBC=0 '-DNULL=((void*)0)' -Wno-unused-function -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-parameter -Wno-implicit-fallthrough -c -o $@ $<

$(NANOJPEG_OBJ_DIR)/nanojpeg_shim.o: $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg_shim.c
	@mkdir -p $(dir $@)
	sed -e 's|"../malloc.h"|"malloc.h"|' -e 's|"../string.h"|"string.h"|' $< | $(CC) $(CFLAGS) -I $(RUNTIME_SRC_DIR) -x c -c -o $@ -

# The keymap source lives at the top-level shared/ directory so the
# kernel's VT keyboard handler and this user-space compositor build
# from the exact same translation logic.
$(APP_OBJ_DIR)/desktop_kbdmap.o: ../shared/kbdmap.c ../shared/kbdmap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I../shared -c -o $@ $<

$(BIN_DIR)/desktop: $(APP_OBJ_DIR)/desktop.o $(APP_OBJ_DIR)/desktop_font.o $(APP_OBJ_DIR)/desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS) $(C_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(C_LINK_OBJS) $(APP_OBJ_DIR)/desktop.o $(APP_OBJ_DIR)/desktop_font.o $(APP_OBJ_DIR)/desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS)

clean:
	rm -rf $(BUILD_ROOT)
