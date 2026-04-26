#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass, field


ARCHES = ("x86", "arm64")


@dataclass(frozen=True)
class SourceMarkers:
    path: str
    markers: tuple[str, ...]


@dataclass(frozen=True)
class TestIntent:
    name: str
    target: str
    commands: dict[str, tuple[str, ...]]
    sources: dict[str, tuple[SourceMarkers, ...]] = field(default_factory=dict)


X86_PMM_KTESTS = (
    "test_alloc_returns_nonzero",
    "test_alloc_page_aligned",
    "test_two_allocs_differ",
    "test_free_count_decrements",
    "test_free_restores_allocability",
    "test_multiple_alloc_and_free",
    "test_refcount_tracks_shared_page",
    "test_refcount_saturates_at_255",
    "test_reserved_pages_are_pinned",
    "test_alloc_never_returns_low_conventional_memory",
    "test_multiboot_framebuffer_reservation_covers_visible_rows",
    "test_multiboot_framebuffer_reservation_rejects_wrap",
    "test_multiboot_usable_ranges_ignore_high_addr_wrap",
)

X86_PROCESS_KTESTS = (
    "test_process_build_initial_frame_layout",
    "test_process_build_exec_frame_layout",
    "test_elf_machine_validation_is_arch_owned",
    "test_sched_add_builds_initial_frame_for_never_run_process",
    "test_process_builds_linux_i386_initial_stack_shape",
    "test_vma_add_keeps_regions_sorted_and_findable",
    "test_vma_add_rejects_overlapping_regions",
    "test_vma_map_anonymous_places_regions_below_stack",
    "test_vma_unmap_range_splits_generic_mapping",
    "test_vma_unmap_range_rejects_heap_or_stack",
    "test_vma_protect_range_splits_and_requires_full_coverage",
    "test_mem_forensics_collects_basic_region_totals",
    "test_mem_forensics_core_note_sizes_are_nonzero",
    "test_core_dump_writes_drunix_notes_in_order",
    "test_mem_forensics_collects_fresh_process_layout",
    "test_mem_forensics_collects_full_vma_table_with_fallback_image",
    "test_mem_forensics_classifies_unmapped_fault",
    "test_mem_forensics_classifies_lazy_miss_for_shadow_heap_mapping",
    "test_mem_forensics_classifies_cow_write_fault",
    "test_mem_forensics_classifies_protection_fault",
    "test_mem_forensics_classifies_unknown_fault_vector",
    "test_mem_forensics_preserves_high_fault_addr_as_unknown",
    "test_mem_forensics_classifies_stack_limit_fault",
    "test_mem_forensics_counts_present_heap_pages",
    "test_process_resources_start_with_single_refs",
    "test_process_resource_get_put_tracks_refs",
    "test_proc_resource_put_exec_owner_releases_solo_owner",
    "test_repeated_exec_owner_put_preserves_heap",
    "test_syscall_fstat_reads_resource_fd_table",
    "test_syscall_getcwd_reads_resource_fs_state",
    "test_syscall_chdir_updates_resource_fs_state",
    "test_syscall_brk_reads_resource_address_space",
    "test_rt_sigaction_reads_resource_handlers",
    "test_clone_rejects_sighand_without_vm",
    "test_clone_rejects_thread_without_sighand",
    "test_clone_thread_shares_group_and_selected_resources",
    "test_clone_process_without_vm_gets_distinct_group_and_as",
    "test_linux_syscalls_fill_uname_time_and_fstat64",
    "test_linux_syscalls_cover_blockdev_fd_path",
    "test_linux_poll_and_select_wait_for_tty_input",
    "test_linux_termios_on_stdout_controls_foreground_tty",
    "test_linux_syscalls_support_busybox_identity_and_rt_sigmask",
    "test_linux_syscalls_support_busybox_stdio_helpers",
    "test_linux_open_create_append_preserves_flags_and_data",
    "test_process_restore_user_tls_switches_global_gdt_slot",
    "test_linux_syscalls_install_tls_and_map_mmap2",
)

X86_UACCESS_KTESTS = (
    "test_copy_from_user_reads_mapped_bytes",
    "test_copy_to_user_spans_pages",
    "test_copy_string_from_user_spans_pages",
    "test_prepare_rejects_kernel_and_unmapped_ranges",
    "test_copy_to_user_breaks_cow_clone",
    "test_release_user_space_drops_only_child_cow_reference",
    "test_process_fork_stack_pages_break_cow_on_write_fault",
    "test_process_fork_child_writes_image_data_before_exec",
    "test_process_fork_child_survives_parent_exit_and_reuses_last_cow_ref",
    "test_process_fork_child_stack_growth_is_private",
    "test_process_fork_child_gets_fresh_task_group_slot",
    "test_process_fork_then_sched_add_child_clears_exec_guard",
    "test_repeated_fork_exec_cleanup_preserves_parent_refs",
    "test_process_fork_bumps_pipe_refs_once_with_resource_table",
    "test_copy_to_user_handles_three_way_cow_sharing",
    "test_process_fork_rolls_back_when_kstack_alloc_fails",
)

X86_DESKTOP_KTESTS = (
    "test_terminal_write_wraps_and_retains_history",
    "test_terminal_render_uses_pixel_padding",
    "test_terminal_render_draws_underline_cursor_in_scrollback_view",
    "test_terminal_render_clips_cursor_to_surface_clip",
    "test_terminal_render_composes_scrollback_before_live_rows",
    "test_terminal_render_uses_ansi_foreground_color",
    "test_terminal_render_uses_ansi_background_color",
    "test_terminal_ansi_color_does_not_emit_escape_bytes",
    "test_terminal_vt_sequences_cover_nano_screen_paint",
    "test_terminal_clear_discards_history_and_resets_cursor",
    "test_terminal_writes_to_later_rows_after_hardening",
    "test_terminal_init_rejects_overflow_dimensions_without_touching_buffers",
    "test_terminal_ansi_digit_sequence_caps_without_overflow",
    "test_terminal_scroll_view_clamps_large_positive_and_negative_inputs",
    "test_terminal_write_rejects_lengths_above_int_max",
    "test_terminal_destroy_preserves_static_buffers_and_clears_owned_buffers",
    "test_gui_display_fill_rect_clips_to_bounds",
    "test_gui_display_draw_text_stops_at_region_edge",
    "test_gui_display_presents_cells_to_framebuffer",
    "test_legacy_console_output_reaches_framebuffer",
    "test_framebuffer_console_handoff_blanks_stale_shadow_background",
    "test_framebuffer_console_handoff_normalizes_imported_text_bg",
    "test_framebuffer_console_handoff_blanks_nonprintable_imports",
    "test_framebuffer_console_handoff_preserves_vga_cursor",
    "test_framebuffer_console_handoff_carries_vga_end_to_next_row",
    "test_vga_text_prompt_after_scrolled_command_output_is_visible",
    "test_vga_text_vt_sequences_cover_nano_screen_paint",
    "test_framebuffer_console_handoff_drops_stale_cells_after_cursor",
    "test_framebuffer_console_uses_columns_beyond_vga_width",
    "test_framebuffer_console_uses_rows_beyond_vga_height",
    "test_boot_cmdline_parses_vgatext_with_nodesktop",
    "test_desktop_files_app_lists_root_entries",
    "test_desktop_files_app_replaces_last_visible_line_when_truncated",
    "test_desktop_help_app_has_keyboard_page",
    "test_desktop_help_app_q_requests_close",
    "test_desktop_app_render_clips_to_content_rect",
    "test_desktop_help_app_render_is_visible_in_framebuffer",
    "test_desktop_help_app_key_input_is_ignored_while_launcher_open",
    "test_desktop_open_invalid_app_kind_is_rejected",
    "test_desktop_processes_app_handles_empty_snapshot",
    "test_desktop_open_processes_refreshes_after_late_process",
    "test_desktop_boot_layout_opens_shell_window",
    "test_desktop_layout_scales_to_framebuffer_grid",
    "test_desktop_render_draws_taskbar_and_launcher_label",
    "test_desktop_launcher_enter_opens_files_window",
    "test_desktop_launcher_enter_does_not_refresh_files_view",
    "test_desktop_framebuffer_launcher_click_opens_files_window",
    "test_desktop_framebuffer_launcher_click_uses_visible_item_rows",
    "test_desktop_text_launcher_keeps_bottom_border_visible",
    "test_desktop_taskbar_click_focuses_processes_window",
    "test_desktop_taskbar_shell_refocus_forwards_keys",
    "test_desktop_text_taskbar_renders_open_window_labels",
    "test_desktop_shell_open_matches_rendered_window_rect",
    "test_desktop_shell_output_still_routes_after_mini_apps_open",
    "test_desktop_close_button_closes_files_window",
    "test_desktop_shell_close_button_closes_visible_shell_window",
    "test_desktop_title_drag_moves_window_and_clamps_top_left",
    "test_desktop_shell_drag_preserves_window_size",
    "test_desktop_shell_drag_syncs_terminal_pixel_rect_in_framebuffer_mode",
    "test_desktop_open_clamps_help_window_to_framebuffer",
    "test_framebuffer_windows_render_in_z_order",
    "test_framebuffer_grid_desktop_renders_taskbar_and_shell_title",
    "test_desktop_init_binds_global_keyboard_target",
    "test_desktop_escape_opens_launcher_and_consumes_input",
    "test_desktop_plain_text_forwards_to_shell_when_focused",
    "test_desktop_write_process_output_targets_shell_surface",
    "test_desktop_child_process_group_output_targets_shell_surface",
    "test_desktop_unrelated_process_group_output_is_rejected",
    "test_syscall_console_write_routes_session_output_to_desktop",
    "test_desktop_ansi_color_escape_updates_attr_without_printing",
    "test_desktop_full_screen_write_does_not_scroll_until_next_char",
    "test_desktop_scroll_syscall_moves_terminal_view",
    "test_desktop_new_output_snaps_scrollback_to_live",
    "test_syscall_clear_clears_desktop_shell_buffer",
    "test_tty_ctrl_c_echo_routes_to_desktop_shell_buffer",
    "test_keyboard_ctrl_letter_translation_matches_raw_tty",
    "test_keyboard_enter_and_tty_icrnl_match_linux",
    "test_keyboard_navigation_scancodes_emit_ansi_sequences",
    "test_mouse_packet_decode_preserves_motion_and_buttons",
    "test_mouse_packet_decode_saturates_overflow_motion",
    "test_mouse_stream_resyncs_after_noise",
    "test_mouse_stream_delivers_response_like_packet_headers",
    "test_mouse_stream_delivers_overflow_packets",
    "test_mouse_stream_keeps_response_like_bytes_inside_packet",
    "test_mouse_irq_drains_initial_packet_without_aux_status",
    "test_mouse_framebuffer_motion_uses_build_configured_scale",
    "test_mouse_text_motion_ignores_framebuffer_speed",
    "test_mouse_overflow_packet_keeps_framebuffer_cursor_visible",
    "test_desktop_pointer_click_focuses_shell_window",
    "test_desktop_pointer_click_ignores_hidden_shell_window",
    "test_desktop_open_builtin_window_focuses_and_reuses_instance",
    "test_desktop_window_focus_raise_updates_z_order",
    "test_desktop_focused_builtin_window_consumes_keys",
    "test_desktop_close_focused_non_shell_window_returns_shell_focus",
    "test_desktop_close_shell_window_stops_key_forwarding",
    "test_desktop_shell_click_focuses_shell_window_table_entry",
    "test_desktop_overlapped_pointer_hits_visible_top_window",
    "test_desktop_render_draws_visible_mouse_pointer",
    "test_desktop_pointer_event_moves_visible_mouse_pointer",
    "test_framebuffer_pointer_motion_does_not_repaint_unrelated_pixels",
    "test_framebuffer_fast_pointer_motion_keeps_cursor_visible_at_edge",
    "test_framebuffer_fast_pointer_motion_repaints_only_cursor_regions",
    "test_framebuffer_window_drag_repaints_only_drag_regions",
    "test_framebuffer_shell_write_repaints_only_dirty_terminal_cells",
    "test_framebuffer_backspace_repaints_only_dirty_terminal_cells",
    "test_framebuffer_newline_repaints_only_dirty_terminal_cells",
    "test_framebuffer_shell_backspace_prompt_redraw_keeps_unrelated_pixels",
    "test_framebuffer_shell_return_prompt_keeps_unrelated_pixels",
    "test_framebuffer_prompt_after_unterminated_output_is_visible",
    "test_framebuffer_prompt_after_scrolled_command_output_is_visible",
    "test_framebuffer_terminal_scroll_keeps_padding_pixels",
    "test_framebuffer_terminal_scroll_does_not_copy_mouse_pointer",
    "test_framebuffer_terminal_scroll_paints_line_written_before_newline",
    "test_framebuffer_mixed_text_scroll_keeps_padding_pixels",
    "test_framebuffer_console_batch_presents_once",
    "test_framebuffer_shell_scroll_keeps_overlapped_window_pixels",
    "test_framebuffer_terminal_scroll_blocks_mouse_pointer_interleave",
    "test_framebuffer_desktop_renders_shell_terminal_background",
    "test_framebuffer_shell_terminal_rect_includes_padding_and_cells",
    "test_framebuffer_shell_window_keeps_right_border_visible",
    "test_framebuffer_desktop_console_output_renders_terminal_glyph",
    "test_framebuffer_desktop_terminal_edges_render_inside_window",
    "test_desktop_can_use_framebuffer_presentation_target",
    "test_framebuffer_info_accepts_1024_768_32_rgb",
    "test_multiboot_framebuffer_color_info_uses_grub_layout",
    "test_boot_framebuffer_grid_clamps_to_static_cell_buffer",
    "test_framebuffer_mapping_reaches_high_physical_lfb",
    "test_framebuffer_info_rejects_address_above_uintptr",
    "test_framebuffer_info_rejects_extent_above_uintptr",
    "test_framebuffer_info_rejects_pitch_overflow_width",
    "test_framebuffer_info_rejects_rgb_mask_past_32_bits",
    "test_framebuffer_info_rejects_overlapping_rgb_masks",
    "test_framebuffer_pack_rgb_uses_mask_positions",
    "test_framebuffer_draw_rect_outline_handles_large_dimensions",
    "test_framebuffer_draw_rect_outline_clips_to_bounds",
    "test_framebuffer_draw_text_clipped_honors_pixel_clip",
    "test_framebuffer_draw_text_clipped_huge_origin_is_noop",
    "test_framebuffer_draw_scrollbar_places_thumb",
    "test_framebuffer_draw_scrollbar_handles_large_row_counts",
    "test_framebuffer_fill_rect_clips_to_bounds",
    "test_framebuffer_fill_rect_handles_large_dimensions",
    "test_framebuffer_draws_pixel_arrow_cursor",
    "test_font8x16_glyph_returns_stable_storage",
    "test_framebuffer_draw_glyph_writes_foreground_pixels",
    "test_framebuffer_draw_glyph_rejects_overflowing_position",
    "test_framebuffer_pack_rgb_scales_to_mask_size",
    "test_k_memset32_fills_words",
)

X86_ARCH_KTESTS = (
    "test_sched_record_user_fault_preserves_full_fault_addr",
)


INTENTS: tuple[TestIntent, ...] = (
    TestIntent(
        name="static include hygiene",
        target="check",
        commands={
            "x86": (
                "python3 tools/compile_commands_sources.py compile_commands.json --under kernel",
            ),
            "arm64": (
                "python3 tools/compile_commands_sources.py compile_commands.json --under kernel",
            ),
        },
    ),
    TestIntent(
        name="interactive shell prompt",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_shell_prompt.py --arch x86",),
            "arm64": ("python3 tools/test_shell_prompt.py --arch arm64",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "tools/test_shell_prompt.py",
                    ("shell-ok",),
                ),
            ),
        },
    ),
    TestIntent(
        name="user program execution",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_user_programs.py --arch x86",),
            "arm64": ("python3 tools/test_user_programs.py --arch arm64",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "tools/test_user_programs.py",
                    (
                        "Hello from C userland!",
                        "Hello from ring 3!",
                        "new[] sum=6",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="sleep syscall behavior",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_sleep.py --arch x86",),
            "arm64": ("python3 tools/test_sleep.py --arch arm64",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "tools/test_sleep.py",
                    ("sleep-ok",),
                ),
            ),
        },
    ),
    TestIntent(
        name="ctrl-c terminal behavior",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_ctrl_c.py --arch x86",),
            "arm64": ("python3 tools/test_ctrl_c.py --arch arm64",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "tools/test_ctrl_c.py",
                    ("prompt-still-alive", "[sleeper] 1"),
                ),
            ),
        },
    ),
    TestIntent(
        name="shell history behavior",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_shell_history.py --arch x86",),
            "arm64": ("python3 tools/test_shell_history.py --arch arm64",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "tools/test_shell_history.py",
                    ("history-recalled-ok", "b\"\\x1b[A\\n\""),
                ),
            ),
        },
    ),
    TestIntent(
        name="kernel unit suite",
        target="test-headless",
        commands={
            "x86": (
                "KTEST=1 kernel disk",
                r"KTEST.*SUMMARY pass=[0-9][0-9]* fail=0",
            ),
            "arm64": ("python3 tools/test_arm64_ktest.py",),
        },
    ),
    TestIntent(
        name="userspace smoke boot",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_user_programs.py --arch x86",),
            "arm64": ("python3 tools/test_arm64_userspace_smoke.py",),
        },
    ),
    TestIntent(
        name="filesystem init",
        target="test-headless",
        commands={
            "x86": ("python3 tools/test_shell_prompt.py --arch x86",),
            "arm64": ("python3 tools/test_arm64_filesystem_init.py",),
        },
    ),
    TestIntent(
        name="syscall parity",
        target="test-headless",
        commands={
            "x86": (
                "KTEST=1 kernel disk",
                r"KTEST.*SUMMARY pass=[0-9][0-9]* fail=0",
            ),
            "arm64": ("python3 tools/test_arm64_syscall_parity.py",),
        },
    ),
    TestIntent(
        name="arch local physical page allocator",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": ("python3 tools/test_arm64_ktest.py",),
        },
        sources={
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_pmm.c",
                    (
                        "test_alloc_returns_nonzero",
                        "test_free_restores_allocability",
                        "test_refcount_tracks_shared_page",
                        "test_alloc_never_returns_low_conventional_memory",
                    ),
                ),
            ),
            "arm64": (
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_vma_map_anonymous_places_regions_below_stack",
                        "test_shared_vma_unmap_range_rejects_heap_or_stack",
                        "test_shared_vma_protect_range_splits_and_requires_full_coverage",
                        "test_shared_copy_to_user_spans_pages",
                        "test_shared_copy_string_from_user_spans_pages",
                        "test_shared_prepare_rejects_kernel_and_unmapped_ranges",
                        "test_shared_proc_resource_put_exec_owner_releases_solo_owner",
                        "test_shared_repeated_exec_owner_put_preserves_heap",
                        "test_shared_syscall_fstat_reads_resource_fd_table",
                        "test_shared_syscall_getcwd_reads_resource_fs_state",
                        "test_shared_syscall_chdir_updates_resource_fs_state",
                        "test_shared_syscall_brk_reads_resource_address_space",
                        "test_shared_rt_sigaction_reads_resource_handlers",
                        "test_shared_clone_rejects_sighand_without_vm",
                        "test_shared_clone_rejects_thread_without_sighand",
                        "test_shared_clone_thread_shares_group_and_selected_resources",
                        "test_shared_clone_process_without_vm_gets_distinct_group_and_as",
                    ),
                ),
                SourceMarkers(
                    "kernel/arch/arm64/test/test_arch_arm64.c",
                    (
                        "test_arm64_pmm_alloc_free_reuses_pages",
                        "test_arm64_pmm_multiple_allocations_are_distinct",
                        "test_arm64_pmm_refcount_tracks_shared_page",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="boot memory map reservation",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": ("python3 tools/test_arm64_ktest.py",),
        },
        sources={
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_pmm.c",
                    (
                        "test_reserved_pages_are_pinned",
                        "test_multiboot_framebuffer_reservation_covers_visible_rows",
                        "test_multiboot_framebuffer_reservation_rejects_wrap",
                        "test_multiboot_usable_ranges_ignore_high_addr_wrap",
                    ),
                ),
            ),
            "arm64": (
                SourceMarkers(
                    "kernel/test/test_pmm_core.c",
                    (
                        "test_pmm_core_respects_reserved_ranges",
                        "test_pmm_core_mark_helpers_update_accounting",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="process launch and userspace execution",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": (
                "python3 tools/test_arm64_userspace_smoke.py",
                "python3 tools/test_arm64_syscall_parity.py",
            ),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "user/arm64init.c",
                    (
                        "ARM64 init: entered",
                        "ARM64 syscall: process ok",
                        "ARM64 init: pass",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_userspace_smoke.py",
                    ("ARM64 user smoke: syscall ok",),
                ),
                SourceMarkers(
                    "tools/test_arm64_syscall_parity.py",
                    ("ARM64 syscall: clone/wait ok",),
                ),
            ),
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_process.c",
                    (
                        "test_process_build_initial_frame_layout",
                        "test_process_build_exec_frame_layout",
                        "test_sched_add_builds_initial_frame_for_never_run_process",
                        "test_process_builds_linux_i386_initial_stack_shape",
                        "test_elf_machine_validation_is_arch_owned",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="process virtual memory bookkeeping",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": ("python3 tools/test_arm64_ktest.py",),
        },
        sources={
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_process.c",
                    (
                        "test_vma_add_keeps_regions_sorted_and_findable",
                        "test_vma_add_rejects_overlapping_regions",
                        "test_vma_map_anonymous_places_regions_below_stack",
                        "test_vma_unmap_range_splits_generic_mapping",
                        "test_vma_protect_range_splits_and_requires_full_coverage",
                    ),
                ),
            ),
            "arm64": (
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_vma_map_anonymous_places_regions_below_stack",
                        "test_shared_vma_unmap_range_rejects_heap_or_stack",
                        "test_shared_vma_protect_range_splits_and_requires_full_coverage",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="process resource ownership",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": ("python3 tools/test_arm64_ktest.py",),
        },
        sources={
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_process.c",
                    (
                        "test_process_resources_start_with_single_refs",
                        "test_process_resource_get_put_tracks_refs",
                        "test_proc_resource_put_exec_owner_releases_solo_owner",
                        "test_repeated_exec_owner_put_preserves_heap",
                    ),
                ),
            ),
            "arm64": (
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_proc_resource_put_exec_owner_releases_solo_owner",
                        "test_shared_repeated_exec_owner_put_preserves_heap",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="process fault forensics",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": (
                "python3 tools/test_arm64_ktest.py",
                "python3 tools/test_arm64_userspace_smoke.py",
            ),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_mem_forensics_collects_basic_region_totals",
                        "test_shared_mem_forensics_core_note_sizes_are_nonzero",
                        "test_shared_mem_forensics_collects_fresh_process_layout",
                        "test_shared_mem_forensics_collects_full_vma_table_with_fallback_image",
                        "test_shared_mem_forensics_counts_present_heap_pages",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_userspace_smoke.py",
                    ("ARM64 user smoke: pass",),
                ),
            ),
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_process.c",
                    (
                        "test_mem_forensics_collects_basic_region_totals",
                        "test_mem_forensics_classifies_unmapped_fault",
                        "test_mem_forensics_classifies_cow_write_fault",
                        "test_core_dump_writes_drunix_notes_in_order",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="user access and copy-on-write memory",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": (
                "python3 tools/test_arm64_userspace_smoke.py",
                "python3 tools/test_arm64_syscall_parity.py",
            ),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "user/arm64init.c",
                    (
                        "ARM64 syscall: memory ok",
                        "ARM64 syscall: clone/wait ok",
                    ),
                ),
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_copy_to_user_spans_pages",
                        "test_shared_copy_string_from_user_spans_pages",
                        "test_shared_prepare_rejects_kernel_and_unmapped_ranges",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_syscall_parity.py",
                    (
                        "ARM64 syscall: memory ok",
                        "ARM64 syscall: clone/wait ok",
                    ),
                ),
            ),
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_uaccess.c",
                    (
                        "test_copy_from_user_reads_mapped_bytes",
                        "test_copy_to_user_spans_pages",
                        "test_copy_string_from_user_spans_pages",
                        "test_prepare_rejects_kernel_and_unmapped_ranges",
                        "test_copy_to_user_breaks_cow_clone",
                        "test_copy_to_user_handles_three_way_cow_sharing",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="fork clone and thread resources",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": ("python3 tools/test_arm64_syscall_parity.py",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "user/arm64init.c",
                    ("ARM64 syscall: clone/wait ok",),
                ),
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_clone_rejects_sighand_without_vm",
                        "test_shared_clone_rejects_thread_without_sighand",
                        "test_shared_clone_thread_shares_group_and_selected_resources",
                        "test_shared_clone_process_without_vm_gets_distinct_group_and_as",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_syscall_parity.py",
                    ("ARM64 syscall: process ok",),
                ),
            ),
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_uaccess.c",
                    (
                        "test_process_fork_child_stack_growth_is_private",
                        "test_process_fork_child_gets_fresh_task_group_slot",
                        "test_process_fork_bumps_pipe_refs_once_with_resource_table",
                        "test_process_fork_rolls_back_when_kstack_alloc_fails",
                    ),
                ),
                SourceMarkers(
                    "kernel/arch/x86/test/test_process.c",
                    (
                        "test_clone_rejects_sighand_without_vm",
                        "test_clone_thread_shares_group_and_selected_resources",
                        "test_clone_process_without_vm_gets_distinct_group_and_as",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="linux syscall compatibility surface",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": ("python3 tools/test_arm64_syscall_parity.py",),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "user/arm64init.c",
                    (
                        "ARM64 syscall: fd/path ok",
                        "ARM64 syscall: signal ok",
                        "ARM64 syscall: utility ok",
                    ),
                ),
                SourceMarkers(
                    "kernel/test/test_arch_shared.c",
                    (
                        "test_shared_syscall_fstat_reads_resource_fd_table",
                        "test_shared_syscall_getcwd_reads_resource_fs_state",
                        "test_shared_syscall_chdir_updates_resource_fs_state",
                        "test_shared_syscall_brk_reads_resource_address_space",
                        "test_shared_rt_sigaction_reads_resource_handlers",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_syscall_parity.py",
                    (
                        "ARM64 syscall: fd/path ok",
                        "ARM64 syscall: signal ok",
                        "ARM64 syscall: utility ok",
                    ),
                ),
            ),
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_process.c",
                    (
                        "test_syscall_fstat_reads_resource_fd_table",
                        "test_syscall_getcwd_reads_resource_fs_state",
                        "test_syscall_chdir_updates_resource_fs_state",
                        "test_syscall_brk_reads_resource_address_space",
                        "test_rt_sigaction_reads_resource_handlers",
                        "test_linux_syscalls_fill_uname_time_and_fstat64",
                        "test_linux_open_create_append_preserves_flags_and_data",
                        "test_linux_syscalls_install_tls_and_map_mmap2",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="busybox compatibility runtime",
        target="test-busybox-compat",
        commands={
            "x86": ("python3 tools/test_busybox_compat.py --arch x86",),
            "arm64": ("python3 tools/test_busybox_compat.py --arch arm64",),
        },
        sources={
            "x86": (
                SourceMarkers(
                    "tools/test_busybox_compat.py",
                    ("BBCOMPAT SUMMARY passed", "build/busybox/x86/busybox"),
                ),
                SourceMarkers(
                    "user/bbcompat.c",
                    ("BBCOMPAT SUMMARY", "sys_execve(\"/bin/busybox\""),
                ),
            ),
            "arm64": (
				SourceMarkers(
					"tools/test_busybox_compat.py",
					(
						"BBCOMPAT SUMMARY passed",
						"build/busybox/arm64/busybox",
						"arm64-smoke",
					),
				),
                SourceMarkers(
                    "user/bbcompat.c",
                    ("BBCOMPAT SUMMARY", "sys_execve(\"/bin/busybox\""),
                ),
            ),
        },
    ),
    TestIntent(
        name="desktop terminal input output behavior",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": (
                "python3 tools/test_shell_prompt.py --arch arm64",
                "python3 tools/test_ctrl_c.py --arch arm64",
                "python3 tools/test_shell_history.py --arch arm64",
            ),
        },
        sources={
            "arm64": (
                SourceMarkers(
                    "tools/test_shell_prompt.py",
                    ("shell-ok",),
                ),
                SourceMarkers(
                    "tools/test_ctrl_c.py",
                    ("prompt-still-alive", "[sleeper] 1"),
                ),
                SourceMarkers(
                    "tools/test_shell_history.py",
                    ("history-recalled-ok",),
                ),
                SourceMarkers(
                    "kernel/test/test_console_terminal.c",
                    ("test_console_terminal_echo_and_backspace",),
                ),
            ),
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_desktop.c",
                    (
                        "test_terminal_write_wraps_and_retains_history",
                        "test_syscall_console_write_routes_session_output_to_desktop",
                        "test_tty_ctrl_c_echo_routes_to_desktop_shell_buffer",
                        "test_keyboard_enter_and_tty_icrnl_match_linux",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="desktop framebuffer and app rendering",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": (
                "python3 tools/test_arm64_ktest.py",
                "python3 tools/test_arm64_filesystem_init.py",
            ),
        },
        sources={
            "x86": (
                SourceMarkers(
                    "kernel/arch/x86/test/test_desktop.c",
                    (
                        "test_gui_display_presents_cells_to_framebuffer",
                        "test_desktop_files_app_lists_root_entries",
                        "test_desktop_render_draws_taskbar_and_launcher_label",
                        "test_framebuffer_info_accepts_1024_768_32_rgb",
                        "test_framebuffer_pack_rgb_uses_mask_positions",
                    ),
                ),
            ),
            "arm64": (
                SourceMarkers(
                    "kernel/test/test_console_terminal.c",
                    (
                        "test_console_terminal_prints_banner_and_help",
                        "test_console_terminal_echo_and_backspace",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_filesystem_init.py",
                    (
                        "Drunix ARM64 console",
                        "drunix shell -- type 'help' for commands",
                    ),
                ),
            ),
        },
    ),
    TestIntent(
        name="x86-only KTEST inventory with narrower ARM64 coverage",
        target="test-headless",
        commands={
            "x86": ("KTEST=1 kernel disk",),
            "arm64": (
                "python3 tools/test_arm64_ktest.py",
                "python3 tools/test_arm64_syscall_parity.py",
                "python3 tools/test_arm64_filesystem_init.py",
            ),
        },
        sources={
            "x86": (
                SourceMarkers("kernel/arch/x86/test/test_pmm.c", X86_PMM_KTESTS),
                SourceMarkers("kernel/arch/x86/test/test_arch_x86.c", X86_ARCH_KTESTS),
                SourceMarkers("kernel/arch/x86/test/test_process.c", X86_PROCESS_KTESTS),
                SourceMarkers("kernel/arch/x86/test/test_uaccess.c", X86_UACCESS_KTESTS),
                SourceMarkers("kernel/arch/x86/test/test_desktop.c", X86_DESKTOP_KTESTS),
            ),
            "arm64": (
                SourceMarkers(
                    "kernel/arch/arm64/test/test_arch_arm64.c",
                    (
                        "test_arm64_pmm_alloc_free_reuses_pages",
                        "test_arm64_pmm_multiple_allocations_are_distinct",
                        "test_arm64_pmm_refcount_tracks_shared_page",
                        "test_arm64_pmm_refcount_saturates_at_255",
                        "test_arm64_vma_add_sorts_and_finds_regions",
                        "test_arm64_vma_add_rejects_overlapping_regions",
                        "test_arm64_vma_unmap_and_protect_split_generic_mapping",
                        "test_arm64_uaccess_copies_mapped_user_bytes",
                        "test_arm64_process_resources_start_with_single_refs",
                        "test_arm64_process_resource_get_put_tracks_refs",
                    ),
                ),
                SourceMarkers(
                    "kernel/test/test_console_terminal.c",
                    (
                        "test_console_terminal_prints_banner_and_help",
                        "test_console_terminal_echo_and_backspace",
                    ),
                ),
                SourceMarkers(
                    "user/arm64init.c",
                    (
                        "ARM64 syscall: memory ok",
                        "ARM64 syscall: fd/path ok",
                        "ARM64 syscall: clone/wait ok",
                        "ARM64 init: pass",
                    ),
                ),
                SourceMarkers(
                    "tools/test_arm64_filesystem_init.py",
                    (
                        "Drunix ARM64 console",
                        "drunix shell -- type 'help' for commands",
                    ),
                ),
            ),
        },
    ),
)
