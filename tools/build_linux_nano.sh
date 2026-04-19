#!/bin/sh
# Build a static Linux i386 GNU nano with a tiny static ncurses dependency.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 OUTPUT [VERSION] [BUILD_DIR]" >&2
    exit 2
fi

out=$1
version=${2:-9.0}
build_dir=${3:-build/nano}
ncurses_version=${NCURSES_VERSION:-6.5}
cc=${LINUX_I386_CC:-i486-linux-musl-gcc}
host=${LINUX_I386_HOST:-i486-linux-musl}
jobs=${JOBS:-2}
nano_url=${NANO_URL:-https://www.nano-editor.org/dist/v9/nano-${version}.tar.xz}
ncurses_url=${NCURSES_URL:-https://ftp.gnu.org/pub/gnu/ncurses/ncurses-${ncurses_version}.tar.gz}

mkdir -p "$build_dir" "$(dirname "$out")"

case "$out" in
    /*) ;;
    *) out="$(pwd)/$out" ;;
esac

build_dir=$(cd "$build_dir" && pwd)
prefix="${build_dir}/i386-root"
fallback_src="${build_dir}/fallback-terminfo.src"
ncurses_archive="${build_dir}/ncurses-${ncurses_version}.tar.gz"
ncurses_src="${build_dir}/ncurses-${ncurses_version}"
nano_archive="${build_dir}/nano-${version}.tar.xz"
nano_src="${build_dir}/nano-${version}"

if [ ! -f "$ncurses_archive" ]; then
    curl -L --fail --retry 3 -o "$ncurses_archive" "$ncurses_url"
fi

if [ ! -d "$ncurses_src" ]; then
    tar -xzf "$ncurses_archive" -C "$build_dir"
fi

need_ncurses=1
if [ -f "${prefix}/lib/libncurses.a" ] &&
   i486-linux-musl-nm "${prefix}/lib/libncurses.a" 2>/dev/null | grep -Eq ' [BDRT] _nc_fallback$' &&
   strings "${prefix}/lib/libncurses.a" | grep -q '^vt100|' &&
   strings "${prefix}/lib/libncurses.a" | grep -q '^vt220|'; then
    need_ncurses=0
fi

if [ "$need_ncurses" -ne 0 ]; then
    infocmp -I vt100 > "$fallback_src"
    infocmp -I vt220 >> "$fallback_src"
    (
        cd "$ncurses_src"
        rm -f ncurses/fallback.c lib/libncurses.a "${prefix}/lib/libncurses.a"
        ./configure \
            --host="$host" \
            --prefix="$prefix" \
            --without-shared \
            --with-normal \
            --without-cxx \
            --without-cxx-binding \
            --without-ada \
            --without-progs \
            --without-tests \
            --without-manpages \
            --without-debug \
            --disable-widec \
            --disable-db-install \
            --with-fallbacks=vt100,vt220 \
            CC="$cc"
        perl -0pi -e "s|^TERMINFO_SRC\\s*=.*$|TERMINFO_SRC = ${fallback_src}|m" ncurses/Makefile
        make -j "$jobs" libs
        make install.libs
        make install.includes
    )
fi

if [ ! -f "$nano_archive" ]; then
    curl -L --fail --retry 3 -o "$nano_archive" "$nano_url"
fi

if [ ! -d "$nano_src" ]; then
    tar -xf "$nano_archive" -C "$build_dir"
fi

(
    cd "$nano_src"
    CPPFLAGS="-I${prefix}/include -I${prefix}/include/ncurses" \
    LDFLAGS="-L${prefix}/lib -static" \
    LIBS="-lncurses" \
    PKG_CONFIG=false \
    ./configure \
        --host="$host" \
        --prefix=/usr \
        --bindir=/bin \
        --enable-tiny \
        --disable-nls \
        --disable-utf8 \
        --without-libmagic \
        CC="$cc"
    make -j "$jobs"
)

cp "${nano_src}/src/nano" "${out}.tmp"
chmod +x "${out}.tmp"
mv "${out}.tmp" "$out"
