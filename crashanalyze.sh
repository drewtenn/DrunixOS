#!/usr/bin/env bash
set -e

CORE="${1:?Usage: crashanalyze.sh <corefile> [user-binary]}"
DISK="${DISK:-disk.img}"

# If the core file doesn't exist locally, try to pull it from the disk image.
if [[ ! -f "$CORE" ]]; then
    if [[ ! -f "$DISK" ]]; then
        echo "error: core file '$CORE' not found and disk image '$DISK' not found" >&2
        exit 1
    fi
    echo "extracting '$CORE' from '$DISK'..."
    python3 tools/dufs_extract.py "$DISK" "$(basename "$CORE")" "$CORE"
    if [[ ! -f "$CORE" ]]; then
        echo "error: extraction failed — '$CORE' still not found" >&2
        exit 1
    fi
fi

# If the caller supplied an explicit binary, use it.  Otherwise extract the
# process name from the NT_PRPSINFO note and look for it under user/.
if [[ -n "$2" ]]; then
    BINARY="$2"
else
    # x86_64-elf-readelf prints the prpsinfo description as raw hex; the
    # pr_fname field starts at byte 32 of the note descriptor (after the
    # 12-byte header: state/sname/zomb/nice + flag + uid/gid + pid/ppid/pgrp/sid).
    # Simpler: grep the note dump for the filename string directly.
    PROGNAME=$(python3 - "$CORE" <<'EOF'
import sys, struct

SECTOR = 512
BLOCK  = 4096
MAGIC  = 0x44554603
INODE_SIZE = 128
INODES_PER_SECTOR = 4
DIRECT_BLOCKS = 12
MAX_NAME = 256
DIRENT_SIZE = 260

def read_prpsinfo_fname(path):
    with open(path, 'rb') as f:
        data = f.read()

    # ELF32 header
    if data[:4] != b'\x7fELF':
        return None
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, = struct.unpack_from('<H', data, 42)
    phnum, = struct.unpack_from('<H', data, 44)

    for i in range(phnum):
        off = phoff + i * phentsize
        ptype, poffset, _, _, pfilesz = struct.unpack_from('<IIIII', data, off)
        if ptype != 4:  # PT_NOTE
            continue
        pos = poffset
        end = poffset + pfilesz
        while pos + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from('<III', data, pos)
            pos += 12
            name_pad = (namesz + 3) & ~3
            desc_pad = (descsz + 3) & ~3
            name = data[pos:pos+namesz].rstrip(b'\x00')
            pos += name_pad
            desc = data[pos:pos+descsz]
            pos += desc_pad
            if name == b'CORE' and ntype == 3 and descsz >= 124:
                # pr_fname is at offset 28 in the prpsinfo struct
                fname = desc[28:44].rstrip(b'\x00').decode('utf-8', errors='replace')
                if fname:
                    print(fname)
                    return
    return None

read_prpsinfo_fname(sys.argv[1])
EOF
)
    if [[ -z "$PROGNAME" ]]; then
        echo "error: could not read process name from core; pass binary explicitly" >&2
        exit 1
    fi
    BINARY="user/$PROGNAME"
    echo "detected binary: $BINARY"
fi

if [[ ! -f "$BINARY" ]]; then
    echo "error: binary '$BINARY' not found" >&2
    exit 1
fi

GDB_IMAGE="osdev-gdb"

if ! docker image inspect "$GDB_IMAGE" &>/dev/null; then
    echo "building $GDB_IMAGE image (one-time setup)..."
    docker build --platform linux/amd64 -t "$GDB_IMAGE" - <<'DOCKERFILE'
FROM ubuntu:22.04
RUN apt-get update -q && apt-get install -y -q --no-install-recommends gdb && rm -rf /var/lib/apt/lists/*
WORKDIR /work
DOCKERFILE
fi

docker run --rm -it \
    --platform linux/amd64 \
    -v "$(pwd)":/work \
    "$GDB_IMAGE" \
    gdb "$BINARY" "$CORE"
