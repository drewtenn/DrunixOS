# Static analysis and style scans for the arm64 build.
ARM_SCAN_FORMAT_SOURCES := $(shell find kernel/arch/arm64 -type f \( -name '*.c' -o -name '*.h' \) -print | sort)
ARM_SCAN_KERNEL_OBJS := $(ARM_KOBJS) $(ARM_SHARED_KOBJS)
ARM_SCAN_USER_C_RUNTIME_OBJS := $(ARM_USER_RUNTIME_OBJ_DIR)/cxx_init.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/syscall.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/malloc.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/string.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/ctype.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/stdlib.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/stdio.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/unistd.o \
                                $(ARM_USER_RUNTIME_OBJ_DIR)/time.o
ARM_SPARSE_CFLAGS := -D__aarch64__ -DDRUNIX_ARM64_VGA=1 \
                     -DDRUNIX_INIT_PROGRAM=\"$(INIT_PROGRAM)\" \
                     -DDRUNIX_INIT_ARG0=\"$(INIT_ARG0)\" \
                     -DDRUNIX_INIT_ENV0=\"$(INIT_ENV0)\" \
                     -DDRUNIX_ROOT_FS=\"$(ROOT_FS)\" \
                     -DDRUNIX_ARM64_SMOKE_FALLBACK=$(ARM64_SMOKE_FALLBACK) \
                     -DDRUNIX_ARM64_HALT_TEST=$(ARM64_HALT_TEST)

compile_commands.json: tools/generate_compile_commands.py kernel/arch/arm64/arch.mk user/programs.mk user/Makefile Makefile mk/scan-arm64.mk FORCE
	$(PYTHON) tools/generate_compile_commands.py \
		--root=. \
		--output=$@ \
		--kernel-objs="$(ARM_SCAN_KERNEL_OBJS)" \
		--kernel-cc="$(ARM_CC)" \
		--kernel-cflags="$(ARM_CFLAGS)" \
		--kernel-inc="$(ARM_INC)" \
		--user-cc="$(ARM_CC)" \
		--user-cflags="$(ARM_USER_CFLAGS) -I user/apps -I user/runtime" \
		--user-build-root="$(ARM_USER_BUILD_DIR)" \
		--user-arch="$(ARCH)" \
		--linux-cc="$(LINUX_ARM64_CC)" \
		--linux-cflags="$(LINUX_CFLAGS)" \
		--user-c-runtime-objs="$(ARM_SCAN_USER_C_RUNTIME_OBJS)" \
		--user-c-progs="$(C_PROGS)" \
		--linux-c-progs="$(LINUX_C_PROGS)"

compile-commands: compile_commands.json

format-check:
	$(call require_tool,$(CLANG_FORMAT))
	@mkdir -p build
	@$(CLANG_FORMAT) --dry-run --Werror $(ARM_SCAN_FORMAT_SOURCES) 2> build/clang-format.log || { \
		sed -n '1,120p' build/clang-format.log; \
		echo "... full clang-format report: build/clang-format.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

cppcheck: compile_commands.json
	$(call require_tool,$(CPPCHECK))
	@mkdir -p build/cppcheck
	@$(CPPCHECK) --project=compile_commands.json \
		--cppcheck-build-dir=build/cppcheck \
		--platform=unix64 \
		--enable=warning \
		--std=c99 \
		--error-exitcode=1 > build/cppcheck.log 2>&1 || { \
		sed -n '1,180p' build/cppcheck.log; \
		echo "... full Cppcheck report: build/cppcheck.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

sparse-check: compile_commands.json
	$(call require_tool,$(SPARSE))
	@mkdir -p build
	@$(PYTHON) tools/compile_commands_sources.py compile_commands.json --under kernel > build/sparse-sources.txt
	@: > build/sparse.log
	@for src in $$(cat build/sparse-sources.txt); do \
		$(SPARSE) $(SPARSEFLAGS) $(ARM_SPARSE_CFLAGS) $(ARM_INC) $$src >> build/sparse.log 2>&1 || true; \
	done; \
	if grep -q "error:" build/sparse.log; then \
		sed -n '1,180p' build/sparse.log; \
		echo "... full Sparse report: build/sparse.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	fi

clang-tidy-include-check: compile_commands.json
	$(call require_tool,$(CLANG_TIDY))
	@mkdir -p build
	@$(PYTHON) tools/compile_commands_sources.py compile_commands.json --under kernel > build/clang-tidy-sources.txt
	@$(CLANG_TIDY) -p compile_commands.json --quiet \
		--checks=-*,misc-include-cleaner \
		--extra-arg=--target=aarch64-none-elf \
		$$(cat build/clang-tidy-sources.txt) > build/clang-tidy-include.log 2>&1 || { \
		sed -n '1,180p' build/clang-tidy-include.log; \
		echo "... full clang-tidy include report: build/clang-tidy-include.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

scan: format-check cppcheck clang-tidy-include-check sparse-check
