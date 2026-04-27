# Static analysis and style scans for the x86 build.
SCAN_C_STYLE_SOURCES := $(shell find kernel -path kernel/test -prune -o -type f \( -name '*.c' -o -name '*.h' \) -print | sort) \
                        $(shell find user -type f \( -name '*.c' -o -name '*.h' \) | sort)
SCAN_KERNEL_C_SOURCES := $(shell find kernel -path kernel/test -prune -o -type f -name '*.c' -print | sort)
SCAN_USER_C_RUNTIME_OBJS := lib/cxx_init.o lib/syscall.o lib/malloc.o \
                            lib/string.o lib/ctype.o lib/stdlib.o \
                            lib/stdio.o lib/unistd.o lib/time.o

compile_commands.json: tools/generate_compile_commands.py kernel/objects.mk user/programs.mk user/Makefile Makefile mk/scan-x86.mk FORCE
	$(PYTHON) tools/generate_compile_commands.py \
		--root=. \
		--output=$@ \
		--kernel-objs="$(KOBJS)" \
		--kernel-cc="$(CC)" \
		--kernel-cflags="$(CFLAGS)" \
		--kernel-inc="$(INC)" \
		--user-cc="$(CC)" \
		--user-cflags="-m32 -ffreestanding -nostdlib -fno-pie -no-pie -fno-stack-protector -fno-omit-frame-pointer -g $(BUILD_OPT) -Wall -Werror -I user/runtime -I shared -I user/apps" \
		--user-build-root="$(USER_BUILD_ROOT)" \
		--user-arch="$(ARCH)" \
		--linux-cc="$(LINUX_I386_CC)" \
		--linux-cflags="$(LINUX_CFLAGS)" \
		--user-c-runtime-objs="$(SCAN_USER_C_RUNTIME_OBJS)" \
		--user-c-progs="$(C_PROGS)" \
		--linux-c-progs="$(LINUX_C_PROGS)"

compile-commands: compile_commands.json

format-check:
	$(call require_tool,$(CLANG_FORMAT))
	@mkdir -p build
	@$(CLANG_FORMAT) --dry-run --Werror $(SCAN_C_STYLE_SOURCES) 2> build/clang-format.log || { \
		sed -n '1,120p' build/clang-format.log; \
		echo "... full clang-format report: build/clang-format.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

cppcheck: compile_commands.json
	$(call require_tool,$(CPPCHECK))
	@mkdir -p build/cppcheck
	@$(CPPCHECK) --project=compile_commands.json \
		--cppcheck-build-dir=build/cppcheck \
		--enable=warning \
		--std=c99 \
		--error-exitcode=1 > build/cppcheck.log 2>&1 || { \
		sed -n '1,180p' build/cppcheck.log; \
		echo "... full Cppcheck report: build/cppcheck.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

sparse-check:
	$(call require_tool,$(SPARSE))
	@mkdir -p build
	@: > build/sparse.log
	@rc=0; for src in $(SCAN_KERNEL_C_SOURCES); do \
		$(SPARSE) $(SPARSEFLAGS) $(CFLAGS) $(INC) $$src >> build/sparse.log 2>&1 || rc=1; \
	done; \
	if [ $$rc -ne 0 ]; then \
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
		--extra-arg=-Wno-unknown-warning-option \
		$$(cat build/clang-tidy-sources.txt) > build/clang-tidy-include.log 2>&1 || { \
		sed -n '1,180p' build/clang-tidy-include.log; \
		echo "... full clang-tidy include report: build/clang-tidy-include.log"; \
		test "$(SCAN_FAIL)" != "1"; \
	}

scan: format-check cppcheck clang-tidy-include-check sparse-check
