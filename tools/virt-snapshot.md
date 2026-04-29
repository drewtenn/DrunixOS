# tools/virt-snapshot.dtb

Frozen device-tree blob from QEMU's `-M virt,gic-version=3` machine.
Used as the input for the FDT-parser KTESTs in
`kernel/arch/arm64/test/test_arch_arm64.c`.

## Generation

QEMU emits a fixed-size buffer when `dumpdtb` is asked, so the output
needs to be trimmed to the real `totalsize` and the header field
re-written. Procedure:

```
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a53 -m 1G \
    -machine dumpdtb=tools/virt-snapshot.dtb -nographic
python3 - <<'EOF'
import struct
with open('tools/virt-snapshot.dtb', 'rb') as f:
    data = f.read()
off_struct = struct.unpack('>I', data[8:12])[0]
off_strings = struct.unpack('>I', data[12:16])[0]
size_strings = struct.unpack('>I', data[32:36])[0]
size_struct = struct.unpack('>I', data[36:40])[0]
real_size = max(off_struct + size_struct, off_strings + size_strings)
real_size = (real_size + 7) & ~7
new_data = bytearray(data[:real_size])
new_data[4:8] = struct.pack('>I', real_size)
with open('tools/virt-snapshot.dtb', 'wb') as f:
    f.write(new_data)
EOF
```

The trimmed blob fits under the 1 MiB sanity ceiling enforced by
`fdt_validate()`.

## Pinned QEMU version

Generated against the QEMU shipped with Homebrew on macOS as of
2026-04-29. The DTB layout is stable across QEMU 8.x and 9.x for the
`virt` machine. Regenerate when adding a new QEMU minor version to
the supported matrix.

## Why frozen

Ship the blob in-tree so KTESTs do not depend on QEMU at build time.
The KTEST suite runs offline against the embedded copy via a small
`.incbin` wrapper.
