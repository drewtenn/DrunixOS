# `test`    — compile with KTEST=1 (in-kernel test suite) and boot under QEMU
#             with the normal display. Tests run silently during boot — their
#             output goes to logs/debugcon.log / /proc/kmsg only, not the on-screen
#             console — so the desktop is visually identical to `make run` and
#             you can inspect visual bugs while tests also executed. Grep
#             `logs/debugcon.log` for `KTEST: SUMMARY pass=N fail=M`.
#             Does NOT rebuild the filesystem.
test:
	$(MAKE) KTEST=1 run

# `test-fresh` — same as `test` but rebuilds the filesystem first.
#                Use when test cases rely on specific files being present on disk.
test-fresh:
	$(MAKE) KTEST=1 run-fresh

# `test-headless` — build with KTEST=1, boot headlessly, wait for the summary
#                   line in logs/debugcon-ktest.log, then exit non-zero if any test
#                   case failed. Use in CI / scripted runs where no human will
#                   look at the framebuffer. Does NOT rebuild the filesystem.
test-headless:
	$(MAKE) KTEST=1 kernel disk
	$(call prepare_test_images,ktest,)
	$(call qemu_headless_until_log,ktest,60,KTEST.*SUMMARY pass=)
	grep -q "KTEST.*SUMMARY pass=" $(LOG_DIR)/debugcon-ktest.log
	grep -q "KTEST.*SUMMARY pass=[0-9][0-9]* fail=0" $(LOG_DIR)/debugcon-ktest.log
	$(MAKE) ARCH=$(ARCH) check-shared-shell check-shared-shell-tests

# `test-halt` — run halt-inducing kernel tests headlessly.  QEMU is launched
#               without a display and killed after a short timeout; the exit
#               status of the target reflects whether the expected panic fired.
#               Currently verifies the double-fault path via a dedicated TSS.
test-halt:
	$(MAKE) DOUBLE_FAULT_TEST=1 kernel disk
	$(call prepare_test_images,df,)
	$(call qemu_headless_for,df,3)
	grep -q "\[PANIC\] --- DOUBLE FAULT ---" $(LOG_DIR)/debugcon-df.log
	grep -q "fault entered through dedicated TSS" $(LOG_DIR)/debugcon-df.log

test-threadtest:
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/threadtest INIT_ARG0=threadtest kernel disk
	$(call prepare_test_images,threadtest,$(LOG_DIR)/threadtest.log)
	$(call qemu_headless_for,threadtest,30)
	$(PYTHON) tools/dufs_extract.py $(IMG_DIR)/dufs-threadtest.img threadtest.log $(LOG_DIR)/threadtest.log
	cat $(LOG_DIR)/threadtest.log
	grep -q "THREADTEST PASS" $(LOG_DIR)/threadtest.log
	! grep -q "THREADTEST FAIL" $(LOG_DIR)/threadtest.log
	! grep -Eq "unknown syscall|Unhandled syscall" $(LOG_DIR)/debugcon-threadtest.log

# `test-ext3-linux-compat` — verify a freshly generated ext3 root with host
#                            e2fsprogs, then boot Drunix writable ext3 smoke
#                            tests and fsck the mutated root image.
test-ext3-linux-compat:
	$(MAKE) validate-ext3-linux
	$(MAKE) KLOG_TO_DEBUGCON=1 INIT_PROGRAM=bin/ext3wtest INIT_ARG0=ext3wtest kernel disk
	$(call prepare_test_images,ext3w,$(LOG_DIR)/ext3wtest.log)
	$(call qemu_headless_for,ext3w,20)
	$(PYTHON) tools/dufs_extract.py $(IMG_DIR)/dufs-ext3w.img ext3wtest.log $(LOG_DIR)/ext3wtest.log
	cat $(LOG_DIR)/ext3wtest.log
	grep -q "EXT3WTEST PASS" $(LOG_DIR)/ext3wtest.log
	dd if=$(IMG_DIR)/disk-ext3w.img of=disk-ext3w.fs bs=512 skip=$(PARTITION_START) count=$(FS_SECTORS) 2>/dev/null
	$(PYTHON) tools/check_ext3_linux_compat.py $(IMG_DIR)/disk-ext3w.img
	$(PYTHON) tools/check_ext3_journal_activity.py $(IMG_DIR)/disk-ext3w.img 1
	$(E2FSCK) -fn disk-ext3w.fs

# `test-ext3-host-write-interop` — use e2fsprogs debugfs to write into the
#                                  generated ext3 image, then read it back and
#                                  fsck the host-mutated image.
test-ext3-host-write-interop:
	$(MAKE) validate-ext3-linux
	mkdir -p $(LOG_DIR) $(IMG_DIR)
	rm -f $(IMG_DIR)/disk-ext3-host.img disk-ext3-host.fs build/ext3-host.txt $(LOG_DIR)/ext3-host-readback.txt
	mkdir -p build
	printf 'linux-host\n' > build/ext3-host.txt
	cp -f disk.fs disk-ext3-host.fs
	$(DEBUGFS) -w -R 'write build/ext3-host.txt linux-host.txt' disk-ext3-host.fs
	$(DEBUGFS) -R 'cat linux-host.txt' disk-ext3-host.fs > $(LOG_DIR)/ext3-host-readback.txt
	grep -q '^linux-host$$' $(LOG_DIR)/ext3-host-readback.txt
	$(PYTHON) tools/wrap_mbr.py disk-ext3-host.fs $(IMG_DIR)/disk-ext3-host.img $(PARTITION_START) $(DISK_SECTORS) 0x83
	$(PYTHON) tools/check_ext3_linux_compat.py $(IMG_DIR)/disk-ext3-host.img
	$(E2FSCK) -fn disk-ext3-host.fs

# `test-all` — run every test suite: in-kernel unit tests (KTEST, headless)
#              followed by all halt-inducing tests.  Exits non-zero if any
#              suite fails.
test-all:
	$(MAKE) test-headless
	$(MAKE) test-threadtest
	$(MAKE) test-halt
