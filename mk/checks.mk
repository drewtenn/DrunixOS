CHECK_GUARDS := check-user-runtime-string check-warning-policy \
                check-arch-boundary-reuse check-start-boundary \
                check-platform-split check-dev-loop-parity \
                check-ext3-root-parity check-shared-shell-tests \
                check-targets-generic check-makefile-decomposition \
                check-test-wiring check-test-intent-coverage

check: clang-tidy-include-check test-headless $(CHECK_GUARDS)

check-phase6:
	python3 tools/test_kernel_arch_boundary_phase6.py

check-phase7:
	python3 tools/test_kernel_arch_boundary_phase7.py

check-shared-shell: check-shell-prompt check-user-programs check-sleep check-ctrl-c check-shell-history

check-shell-prompt:
	python3 tools/test_shell_prompt.py --arch $(ARCH)

check-user-programs:
	python3 tools/test_user_programs.py --arch $(ARCH)

check-sleep:
	python3 tools/test_sleep.py --arch $(ARCH)

check-ctrl-c:
	python3 tools/test_ctrl_c.py --arch $(ARCH)

check-shell-history:
	python3 tools/test_shell_history.py --arch $(ARCH)

ifeq ($(ARCH),arm64)
check-userspace-smoke:
	python3 tools/test_arm64_userspace_smoke.py

check-filesystem-init:
	python3 tools/test_arm64_filesystem_init.py

check-kernel-unit:
	python3 tools/test_arm64_ktest.py --platform raspi3b
	python3 tools/test_arm64_ktest.py --platform virt

check-syscall-parity:
	python3 tools/test_arm64_syscall_parity.py

test-ext3-linux-compat test-ext3-host-write-interop validate-ext3-linux:
	@echo "make ARCH=arm64 $@ is not implemented yet"
	@exit 2
else
check-userspace-smoke:
	python3 tools/test_user_programs.py --arch x86

check-filesystem-init:
	python3 tools/test_shell_prompt.py --arch x86

check-kernel-unit: test-headless

check-syscall-parity:
	$(MAKE) ARCH=x86 test-headless

validate-ext3-linux: $(ROOT_DISK_IMG) tools/check_ext3_linux_compat.py tools/check_ext3_journal_activity.py
	$(PYTHON) tools/check_ext3_linux_compat.py $(ROOT_DISK_IMG)
	$(E2FSCK) -fn disk.fs
	$(DUMPE2FS) -h disk.fs | grep -q 'Filesystem features:.*has_journal'
	$(DUMPE2FS) -h disk.fs | grep -q 'Journal inode:[[:space:]]*8'
endif

check-user-runtime-string:
	python3 tools/test_user_runtime_string_fastpaths.py

check-busybox-compat:
	python3 tools/test_busybox_compat.py --arch $(ARCH)

test-busybox-compat: check-busybox-compat

check-arch-boundary-reuse:
	python3 tools/test_arch_boundary_reuse.py

check-start-boundary:
	python3 tools/test_target_specific_sources_arch_local.py

check-platform-split:
	python3 tools/test_arm64_platform_split.py

check-dev-loop-parity:
	python3 tools/test_arm64_dev_loop_parity.py

check-ext3-root-parity:
	python3 tools/test_arm64_ext3_root_parity.py

check-shared-shell-tests:
	python3 tools/test_shared_shell_tests_arch_neutral.py

check-targets-generic:
	python3 tools/test_make_targets_arch_neutral.py

check-makefile-decomposition:
	python3 tools/test_makefile_decomposition.py

check-warning-policy:
	python3 tools/test_warning_policy.py

check-test-wiring:
	python3 tools/test_check_wiring.py --arch $(ARCH)

check-test-intent-coverage:
	python3 tools/check_test_intent_coverage.py
