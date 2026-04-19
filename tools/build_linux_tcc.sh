#!/bin/sh
# Build a static Linux i386 TinyCC with the musl cross compiler.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 OUTPUT [VERSION] [BUILD_DIR]" >&2
    exit 2
fi

out=$1
version=${2:-0.9.27}
build_dir=${3:-build/tcc}
cc=${LINUX_I386_CC:-i486-linux-musl-gcc}
jobs=${JOBS:-2}
url=${TCC_URL:-https://download.savannah.gnu.org/releases/tinycc/tcc-${version}.tar.bz2}
cross_prefix=${cc%gcc}

archive="${build_dir}/tcc-${version}.tar.bz2"
src="${build_dir}/tcc-${version}"

mkdir -p "$build_dir" "$(dirname "$out")"

tmpbin=$(mktemp -d "${TMPDIR:-/tmp}/tcc-build.XXXXXX")
trap 'rm -rf "$tmpbin"' EXIT HUP INT TERM

cat >"$tmpbin/uname" <<'EOF'
#!/bin/sh
case "$1" in
    -s|'')
        echo Linux
        ;;
    -m)
        echo i386
        ;;
    *)
        /usr/bin/uname "$@"
        ;;
esac
EOF
chmod +x "$tmpbin/uname"

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

PATH="$tmpbin:$PATH" ./configure \
    --cc=gcc \
    --cross-prefix="$cross_prefix" \
    --cpu=i386 \
    --triplet=i386-linux-musl \
    --config-musl \
    --prefix=/usr \
    --bindir=/bin \
    --libdir=/usr/lib \
    --includedir=/usr/include \
    --extra-cflags="-static -Os" \
    --extra-ldflags="-static"

make -j "$jobs" tcc

cp tcc "${out}.tmp"
mv "${out}.tmp" "$out"
