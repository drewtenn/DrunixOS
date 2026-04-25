#!/bin/sh
# Build a static Linux BusyBox with a musl cross compiler.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 OUTPUT [VERSION] [BUILD_DIR]" >&2
    exit 2
fi

out=$1
version=${2:-1.36.1}
build_dir=${3:-build/busybox}
cross_compile=${BUSYBOX_CROSS_COMPILE:-${LINUX_I386_CROSS_COMPILE:-i486-linux-musl-}}
cc=${BUSYBOX_CC:-${LINUX_I386_CC:-i486-linux-musl-gcc}}
ldflags=${BUSYBOX_LDFLAGS:-}
jobs=${JOBS:-2}
url=${BUSYBOX_URL:-https://busybox.net/downloads/busybox-${version}.tar.bz2}

archive="${build_dir}/busybox-${version}.tar.bz2"
src="${build_dir}/busybox-${version}"

mkdir -p "$build_dir" "$(dirname "$out")"

case "$out" in
    /*) ;;
    *) out="$(pwd)/$out" ;;
esac

if [ ! -f "$archive" ]; then
    curl -L --fail --retry 3 -o "$archive" "$url"
fi

if [ ! -d "$src" ]; then
    tar -xf "$archive" -C "$build_dir"
fi

cd "$src"

if [ ! -f .config ]; then
    make defconfig >/dev/null
fi

if ! grep -q '^CONFIG_STATIC=y$' .config; then
    perl -0pi -e 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    yes '' | make oldconfig >/dev/null
fi

make -j "$jobs" CROSS_COMPILE="$cross_compile" CC="$cc" LDFLAGS="$ldflags" busybox

cp busybox "${out}.tmp"
mv "${out}.tmp" "$out"
