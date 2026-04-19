#!/bin/sh
# Build selected static Linux i386 GNU binutils with the musl cross compiler.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 OUTPUT [VERSION] [BUILD_DIR]" >&2
    exit 2
fi

out=$1
version=${2:-2.42}
build_dir=${3:-build/binutils}
cc=${LINUX_I386_CC:-i486-linux-musl-gcc}
jobs=${JOBS:-2}
url=${BINUTILS_URL:-https://ftp.gnu.org/gnu/binutils/binutils-${version}.tar.xz}
tool=$(basename "$out")
binutils_tool=$tool
if [ "$tool" = "gcc-as" ]; then
    binutils_tool=as
fi

case "$binutils_tool" in
    readelf|objdump|as)
        ;;
    *)
        echo "unsupported binutils tool: $tool" >&2
        exit 2
        ;;
esac

mkdir -p "$build_dir" "$(dirname "$out")"
build_dir=$(cd "$build_dir" && pwd)
archive="${build_dir}/binutils-${version}.tar.xz"
src="${build_dir}/binutils-${version}"
obj="${build_dir}/obj-i386-linux-musl-${version}"
lock="${build_dir}/.binutils.lock"

case "$out" in
    /*) ;;
    *) out="$(pwd)/$out" ;;
esac

while ! mkdir "$lock" 2>/dev/null; do
    sleep 1
done
trap 'rmdir "$lock"' EXIT INT TERM

if [ ! -f "$archive" ]; then
    curl -L --fail --retry 3 -o "$archive" "$url"
fi

if [ ! -d "$src" ]; then
    tar -xf "$archive" -C "$build_dir"
fi

mkdir -p "$obj"
cd "$obj"

if [ ! -f Makefile ]; then
    MAKEINFO=true "../binutils-${version}/configure" \
        --host=i486-linux-musl \
        --target=i386-linux-musl \
        --disable-nls \
        --disable-shared \
        --enable-static \
        --disable-werror \
        --without-zstd \
        CC="$cc" \
        CFLAGS="-static -Os" \
        LDFLAGS="-static"
fi

case "$binutils_tool" in
    as)
        make -j "$jobs" MAKEINFO=true all-gas
        rm -f gas/as-new
        make -C gas MAKEINFO=true LDFLAGS="-all-static -static" as-new
        cp gas/as-new "${out}.tmp"
        ;;
    *)
        make -j "$jobs" MAKEINFO=true all-binutils
        rm -f "binutils/${tool}"
        make -C binutils MAKEINFO=true LDFLAGS="-all-static -static" "$tool"
        cp "binutils/${tool}" "${out}.tmp"
        ;;
esac
mv "${out}.tmp" "$out"
