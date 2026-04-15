# Mouse Speed Build Config Design

## Goal

Add a build-time mouse cursor speed option for DrunixOS. The current framebuffer
mouse speed is hard-coded in `kernel/drivers/mouse.c` as a pixel-motion scale.
The new option should preserve the current default behavior while allowing a
developer to build a faster or slower cursor without editing source.

## User Interface

The build interface is a Make variable:

```sh
make MOUSE_SPEED=6 os.iso
```

If `MOUSE_SPEED` is omitted, the default value is `4`, matching the current
framebuffer cursor speed.

## Architecture

`Makefile` owns the build-time option and passes it into kernel C compilation
as a preprocessor definition. `kernel/drivers/mouse.c` uses that definition in
place of the existing hard-coded framebuffer scale.

The scale applies only when the desktop is backed by a framebuffer. The VGA/text
fallback keeps scale `1`, preserving current cell-based pointer behavior.

## Validation

The C side guards against unusable values:

- Values below `1` are treated as `1`.
- Values above `16` are treated as `16`.

The upper bound is named near the mouse config constants so tests and future
changes have a single obvious place to look.

## Tests

Add KTEST coverage for:

- The default framebuffer scale remains `4`.
- A compiled override changes the framebuffer scale used by
  `mouse_update_pointer_for_test`.
- The text/VGA path remains scale `1`.
- Invalid low and high values clamp to the supported range.

Because this is a compile-time knob, the normal KTEST run should cover the
default value. A targeted build can verify an override, for example:

```sh
make KTEST=1 MOUSE_SPEED=6 os.iso
```

## Documentation

Update `README.md` with the new build option, because repository rules say
configuration changes should be reflected there.

## Out Of Scope

Runtime mouse speed changes, shell commands, config files on disk, acceleration
curves, and per-axis tuning are out of scope for this change.
