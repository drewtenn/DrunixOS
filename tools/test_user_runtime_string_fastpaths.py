#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STRING_C = ROOT / "user" / "runtime" / "string.c"


HARNESS = r"""
typedef __SIZE_TYPE__ size_t;

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

static int check_memcpy_case(size_t dst_off, size_t src_off, size_t n)
{
	unsigned char src[96];
	unsigned char dst[96];
	unsigned char ref[96];
	size_t i;

	for (i = 0; i < sizeof(src); i++)
		src[i] = (unsigned char)(i * 7u + 3u);
	for (i = 0; i < sizeof(dst); i++) {
		dst[i] = 0xa5u;
		ref[i] = 0xa5u;
	}
	for (i = 0; i < n; i++)
		ref[dst_off + i] = src[src_off + i];

	if (memcpy(dst + dst_off, src + src_off, n) != dst + dst_off)
		return 1;
	for (i = 0; i < sizeof(dst); i++)
		if (dst[i] != ref[i])
			return 1;
	return 0;
}

static int check_memset_case(size_t off, int c, size_t n)
{
	unsigned char buf[96];
	unsigned char ref[96];
	size_t i;

	for (i = 0; i < sizeof(buf); i++) {
		buf[i] = (unsigned char)(i + 11u);
		ref[i] = (unsigned char)(i + 11u);
	}
	for (i = 0; i < n; i++)
		ref[off + i] = (unsigned char)c;

	if (memset(buf + off, c, n) != buf + off)
		return 1;
	for (i = 0; i < sizeof(buf); i++)
		if (buf[i] != ref[i])
			return 1;
	return 0;
}

int main(void)
{
	if (check_memcpy_case(0u, 0u, 32u))
		return 1;
	if (check_memcpy_case(1u, 1u, 31u))
		return 2;
	if (check_memcpy_case(2u, 2u, 19u))
		return 3;
	if (check_memcpy_case(0u, 1u, 23u))
		return 4;
	if (check_memset_case(0u, 0x5au, 32u))
		return 5;
	if (check_memset_case(1u, 0xc3u, 31u))
		return 6;
	if (check_memset_case(2u, 0x00u, 19u))
		return 7;
	return 0;
}
"""


def require_fastpath_markers() -> None:
	text = STRING_C.read_text()
	for name in ("memcpy", "memset"):
		start = text.index(f"void *{name}(")
		end = text.index("\n}", start)
		body = text[start:end]
		missing = [
			marker
			for marker in ("uintptr_t", "uint32_t", "sizeof(uint32_t)")
			if marker not in body
		]
		if missing:
			raise AssertionError(
				f"{STRING_C.relative_to(ROOT)} {name} lacks aligned 32-bit "
				f"fast-path markers: {', '.join(missing)}"
			)


def compile_and_run_harness() -> None:
	with tempfile.TemporaryDirectory() as tmp:
		tmp_path = Path(tmp)
		harness = tmp_path / "runtime_string_harness.c"
		bin_path = tmp_path / "runtime_string_harness"
		harness.write_text(HARNESS)
		cmd = [
			"cc",
			"-std=c99",
			"-Wall",
			"-Wextra",
			"-Werror",
			"-fno-builtin",
			"-I",
			str(ROOT / "user" / "runtime"),
			str(STRING_C),
			str(harness),
			"-o",
			str(bin_path),
		]
		subprocess.run(cmd, cwd=ROOT, check=True)
		subprocess.run([str(bin_path)], cwd=ROOT, check=True)


def main() -> int:
	try:
		require_fastpath_markers()
		compile_and_run_harness()
	except (AssertionError, subprocess.CalledProcessError) as exc:
		print(exc)
		return 1
	print("user runtime string fast paths passed")
	return 0


if __name__ == "__main__":
	sys.exit(main())
