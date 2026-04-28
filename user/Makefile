include programs.mk

USER_ARCH ?= x86
USER_LOAD_ADDR ?= 0x08048000
BUILD_ROOT ?= ../build/user/$(USER_ARCH)
BIN_DIR := $(BUILD_ROOT)/bin
OBJ_DIR := $(BUILD_ROOT)/obj
RUNTIME_DIR := $(BUILD_ROOT)/runtime
LINKER_DIR := $(BUILD_ROOT)/linker
APP_SRC_DIR := apps
RUNTIME_SRC_DIR := runtime
THIRD_PARTY_SRC_DIR := third_party
LINKER_SRC_DIR := linker
REXC_SRC_DIR := apps

APP_OBJ_DIR := $(OBJ_DIR)/apps
RUNTIME_OBJ_DIR := $(OBJ_DIR)/runtime
THIRD_PARTY_OBJ_DIR := $(OBJ_DIR)/third_party
NANOJPEG_OBJ_DIR := $(THIRD_PARTY_OBJ_DIR)/nanojpeg

CLEAN_PROGS = $(addprefix $(BIN_DIR)/,$(PROGS))

all: $(CLEAN_PROGS) $(RUNTIME_DIR)/libc.a

.PHONY: all clean print-progs $(PROGS)

print-progs:
	@printf '%s\n' $(PROGS)

CC     = x86_64-elf-gcc
CXX    = x86_64-elf-g++
LD     = x86_64-elf-ld
AR     = x86_64-elf-ar
NASM   = nasm
REXC_BUILD_DIR ?= ../external/rexc/build
REXC  ?= $(REXC_BUILD_DIR)/rexc
BUILD_MODE ?= production
ifeq ($(BUILD_MODE),debug)
BUILD_OPT ?= -Og
else ifeq ($(BUILD_MODE),production)
BUILD_OPT ?= -O2
else
$(error BUILD_MODE must be production or debug)
endif
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
         -fno-stack-protector -fno-omit-frame-pointer -g $(BUILD_OPT) -Wall \
         -Werror -fdebug-prefix-map=$(abspath ..)=.
CXXFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -no-pie \
           -fno-stack-protector -fno-omit-frame-pointer -g $(BUILD_OPT) -Wall \
           -Werror -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
           -fno-threadsafe-statics -fdebug-prefix-map=$(abspath ..)=.
NASMFLAGS = -Werror
CXXLIBS = $(shell $(CXX) -m32 -print-libgcc-file-name)
USER_INCLUDES = -I $(RUNTIME_SRC_DIR) -I ../shared -I $(APP_SRC_DIR) -I $(THIRD_PARTY_SRC_DIR)/nanojpeg
DEPFLAGS = -MMD -MP -MF $(@:.o=.d)
BUILD_MODE_FLAG := $(BUILD_ROOT)/.build-mode-flag

C_RUNTIME_OBJS = $(RUNTIME_OBJ_DIR)/crt0.o $(RUNTIME_OBJ_DIR)/cxx_init.o $(RUNTIME_OBJ_DIR)/syscall.o $(RUNTIME_OBJ_DIR)/malloc.o \
                 $(RUNTIME_OBJ_DIR)/string.o $(RUNTIME_OBJ_DIR)/ctype.o $(RUNTIME_OBJ_DIR)/stdlib.o $(RUNTIME_OBJ_DIR)/stdio.o \
                 $(RUNTIME_OBJ_DIR)/unistd.o $(RUNTIME_OBJ_DIR)/time.o
C_RUNTIME_LIB_OBJS = $(filter-out $(RUNTIME_OBJ_DIR)/crt0.o,$(C_RUNTIME_OBJS))
CXX_RUNTIME_OBJS = $(RUNTIME_OBJ_DIR)/cxxrt.o $(RUNTIME_OBJ_DIR)/cxxabi.o
C_LINK_OBJS = $(C_RUNTIME_OBJS)
CXX_LINK_OBJS = $(C_RUNTIME_OBJS) $(CXX_RUNTIME_OBJS)

C_GENERIC_PROGS = $(filter-out desktop,$(C_PROGS))
CXX_NATIVE_PROGS = $(filter-out $(REXC_PROGS),$(CXX_PROGS))
C_BINS = $(addprefix $(BIN_DIR)/,$(C_GENERIC_PROGS))
REXC_BINS = $(addprefix $(BIN_DIR)/,$(REXC_PROGS))
CXX_BINS = $(addprefix $(BIN_DIR)/,$(CXX_NATIVE_PROGS))
C_APP_OBJS = $(addprefix $(APP_OBJ_DIR)/,$(addsuffix .o,$(C_PROGS)))
CXX_APP_OBJS = $(addprefix $(APP_OBJ_DIR)/,$(addsuffix .o,$(CXX_NATIVE_PROGS)))

$(PROGS): %: $(BIN_DIR)/%

$(REXC):
	cmake -S ../external/rexc -B $(REXC_BUILD_DIR)
	cmake --build $(REXC_BUILD_DIR) --target rexc

$(LINKER_DIR)/user.ld: $(LINKER_SRC_DIR)/user.ld.in
	@mkdir -p $(dir $@)
	sed 's|@USER_LOAD_ADDR@|$(USER_LOAD_ADDR)|' $< > $@

FORCE:

$(BUILD_MODE_FLAG): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n%s\n' "$(BUILD_MODE)" "$(BUILD_OPT)" | cmp -s - $@ || printf '%s\n%s\n' "$(BUILD_MODE)" "$(BUILD_OPT)" > $@

$(RUNTIME_OBJ_DIR)/crt0.o: $(RUNTIME_SRC_DIR)/crt0.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: $(RUNTIME_SRC_DIR)/%.c $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(RUNTIME_OBJ_DIR)/%.o: $(RUNTIME_SRC_DIR)/%.cpp $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.c $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(USER_INCLUDES) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.cpp $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(USER_INCLUDES) -c -o $@ $<

$(APP_OBJ_DIR)/%.o: $(APP_SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -f elf32 -o $@ $<

$(RUNTIME_DIR)/libc.a: $(C_RUNTIME_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(C_RUNTIME_LIB_OBJS)

$(C_BINS): $(BIN_DIR)/%: $(APP_OBJ_DIR)/%.o $(C_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(C_LINK_OBJS) $(APP_OBJ_DIR)/$*.o

$(REXC_BINS): $(BIN_DIR)/%: $(REXC_SRC_DIR)/%.rx $(REXC) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(REXC) $< --target i386-drunix --drunix-root .. -o $@

$(CXX_BINS): $(BIN_DIR)/%: $(APP_OBJ_DIR)/%.o $(CXX_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(CXX_LINK_OBJS) $(APP_OBJ_DIR)/$*.o $(CXXLIBS)

DESKTOP_NANOJPEG_OBJS = $(NANOJPEG_OBJ_DIR)/nanojpeg.o $(NANOJPEG_OBJ_DIR)/nanojpeg_shim.o

$(NANOJPEG_OBJ_DIR)/nanojpeg.o: $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg.c $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg.h $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I $(RUNTIME_SRC_DIR) -DNJ_USE_LIBC=0 '-DNULL=((void*)0)' -Wno-unused-function -Wno-sign-compare -Wno-misleading-indentation -Wno-unused-parameter -Wno-implicit-fallthrough -c -o $@ $<

$(NANOJPEG_OBJ_DIR)/nanojpeg_shim.o: $(THIRD_PARTY_SRC_DIR)/nanojpeg/nanojpeg_shim.c $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I $(RUNTIME_SRC_DIR) -c -o $@ $<

# The keymap source lives at the top-level shared/ directory so the
# kernel's VT keyboard handler and this user-space compositor build
# from the exact same translation logic.
$(APP_OBJ_DIR)/desktop_kbdmap.o: ../shared/kbdmap.c ../shared/kbdmap.h $(BUILD_MODE_FLAG)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I../shared -c -o $@ $<

$(BIN_DIR)/desktop: $(APP_OBJ_DIR)/desktop.o $(APP_OBJ_DIR)/desktop_font.o $(APP_OBJ_DIR)/desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS) $(C_LINK_OBJS) $(LINKER_DIR)/user.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LINKER_DIR)/user.ld -o $@ $(C_LINK_OBJS) $(APP_OBJ_DIR)/desktop.o $(APP_OBJ_DIR)/desktop_font.o $(APP_OBJ_DIR)/desktop_kbdmap.o $(DESKTOP_NANOJPEG_OBJS)

clean:
	rm -rf $(BUILD_ROOT)
	find . -name '*.o' -delete
	find . -name '*.elf' -delete
	$(RM) $(PROGS)
	$(RM) linuxhello linuxprobe linuxabi readelf objdump gcc gcc-cc1 gcc-as tcc
	$(RM) user.ld user_arm64.ld lib/libc.a

USER_DEPS = $(C_RUNTIME_OBJS:.o=.d) $(CXX_RUNTIME_OBJS:.o=.d) \
            $(C_APP_OBJS:.o=.d) $(CXX_APP_OBJS:.o=.d) \
            $(DESKTOP_NANOJPEG_OBJS:.o=.d) $(APP_OBJ_DIR)/desktop_kbdmap.d

-include $(USER_DEPS)
