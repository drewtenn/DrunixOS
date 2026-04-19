#!/bin/sh
# Stage selected static Linux i686 GCC toolchain programs from a prebuilt musl.cc tarball.

set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 OUTPUT TOOL [VERSION] [BUILD_DIR]" >&2
    exit 2
fi

out=$1
tool=$2
version=${3:-11.2.1}
build_dir=${4:-build/gcc}
url=${GCC_TOOLCHAIN_URL:-https://musl.cc/i686-linux-musl-cross.tgz}

mkdir -p "$build_dir" "$(dirname "$out")"
build_dir=$(cd "$build_dir" && pwd)
archive="${build_dir}/i686-linux-musl-cross.tgz"
src="${build_dir}/i686-linux-musl-cross"
lock="${build_dir}/.gcc-toolchain.lock"

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
    tar -xzf "$archive" -C "$build_dir"
fi

case "$tool" in
    gcc)
        src_file="${src}/bin/i686-linux-musl-gcc"
        ;;
    cc1)
        src_file="${src}/libexec/gcc/i686-linux-musl/${version}/cc1"
        ;;
    as)
        src_file="${src}/i686-linux-musl/bin/as"
        ;;
    *)
        echo "unsupported GCC tool: $tool" >&2
        exit 2
        ;;
esac

if [ ! -x "$src_file" ]; then
    echo "missing GCC tool: $src_file" >&2
    exit 1
fi

cp "$src_file" "${out}.tmp"
chmod +x "${out}.tmp"
mv "${out}.tmp" "$out"
