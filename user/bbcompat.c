/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * bbcompat.c - unattended BusyBox/Linux i386 compatibility runner.
 */

#include "lib/stdio.h"
#include "lib/string.h"
#include "lib/syscall.h"

#define BBCOMPAT_TOTAL 255
#define BB_OUT_CAP 4096

typedef struct {
    const char *name;
    int expected_exit;
    const char *must_contain;
    const char *stdin_text;
    const char *argv[7];
} bb_case_t;

#define BB_CASE(name, code, contains, input, a0, a1, a2, a3, a4, a5) \
    { name, code, contains, input, { a0, a1, a2, a3, a4, a5, 0 } }

static const bb_case_t cases[] = {
    BB_CASE("true exits zero", 0, 0, 0,
            "true", 0, 0, 0, 0, 0),
    BB_CASE("false exits one", 1, 0, 0,
            "false", 0, 0, 0, 0, 0),
    BB_CASE("echo writes a line", 0, "hello\n", 0,
            "echo", "hello", 0, 0, 0, 0),
    BB_CASE("echo -n suppresses newline", 0, "tight", 0,
            "echo", "-n", "tight", 0, 0, 0),
    BB_CASE("printf formats strings", 0, "fmt ok\n", 0,
            "printf", "%s %s\n", "fmt", "ok", 0, 0),
    BB_CASE("printf formats numbers", 0, "7\n", 0,
            "printf", "%d\n", "7", 0, 0, 0),
    BB_CASE("uname reports Drunix", 0, "Drunix", 0,
            "uname", "-s", 0, 0, 0, 0),
    BB_CASE("pwd reports root", 0, "/", 0,
            "pwd", 0, 0, 0, 0, 0),
    BB_CASE("basename trims path", 0, "busybox\n", 0,
            "basename", "/usr/bin/busybox", 0, 0, 0, 0),
    BB_CASE("dirname trims leaf", 0, "/usr/bin\n", 0,
            "dirname", "/usr/bin/busybox", 0, 0, 0, 0),
    BB_CASE("dirname relative leaf", 0, ".\n", 0,
            "dirname", "hello.txt", 0, 0, 0, 0),
    BB_CASE("expr adds integers", 0, "3\n", 0,
            "expr", "1", "+", "2", 0, 0),
    BB_CASE("expr string length", 0, "3\n", 0,
            "expr", "length", "abc", 0, 0, 0),
    BB_CASE("test nonempty string", 0, 0, 0,
            "test", "-n", "x", 0, 0, 0),
    BB_CASE("test empty string", 0, 0, 0,
            "test", "-z", "", 0, 0, 0),
    BB_CASE("test integer equality", 0, 0, 0,
            "test", "1", "-eq", "1", 0, 0),
    BB_CASE("seq counts upward", 0, "1\n2\n3\n", 0,
            "seq", "1", "3", 0, 0, 0),
    BB_CASE("sleep zero returns", 0, 0, 0,
            "sleep", "0", 0, 0, 0, 0),
    BB_CASE("cat reads hello file", 0, "Hello from", 0,
            "cat", "hello.txt", 0, 0, 0, 0),
    BB_CASE("cat reads readme file", 0, "Line three", 0,
            "cat", "readme.txt", 0, 0, 0, 0),
    BB_CASE("wc counts readme lines", 0, "readme.txt", 0,
            "wc", "-l", "readme.txt", 0, 0, 0),
    BB_CASE("wc counts stdin bytes", 0, "4", "abcd",
            "wc", "-c", 0, 0, 0, 0),
    BB_CASE("head reads first line", 0, "DUFS test file.", 0,
            "head", "-n", "1", "readme.txt", 0, 0),
    BB_CASE("head reads stdin", 0, "first\n", "first\nsecond\n",
            "head", "-n", "1", 0, 0, 0),
    BB_CASE("tail reads last line", 0, "Line three.", 0,
            "tail", "-n", "1", "readme.txt", 0, 0),
    BB_CASE("tail reads stdin", 0, "second\n", "first\nsecond\n",
            "tail", "-n", "1", 0, 0, 0),
    BB_CASE("grep finds file line", 0, "Line two.", 0,
            "grep", "Line two", "readme.txt", 0, 0, 0),
    BB_CASE("grep quiet match", 0, 0, 0,
            "grep", "-q", "DUFS", "readme.txt", 0, 0),
    BB_CASE("grep filters stdin", 0, "needle\n", "hay\nneedle\n",
            "grep", "needle", 0, 0, 0, 0),
    BB_CASE("grep invert match", 0, "keep\n", "keep\nskip\n",
            "grep", "-v", "skip", 0, 0, 0),
    BB_CASE("sort orders stdin", 0, "a\nb\nc\n", "c\na\nb\n",
            "sort", 0, 0, 0, 0, 0),
    BB_CASE("uniq collapses stdin", 0, "a\nb\n", "a\na\nb\n",
            "uniq", 0, 0, 0, 0, 0),
    BB_CASE("cut selects characters", 0, "abc\n", "abcdef\n",
            "cut", "-c1-3", 0, 0, 0, 0),
    BB_CASE("tr uppercases stdin", 0, "ABC\n", "abc\n",
            "tr", "a-z", "A-Z", 0, 0, 0),
    BB_CASE("sed substitutes stdin", 0, "bar\n", "foo\n",
            "sed", "s/foo/bar/", 0, 0, 0, 0),
    BB_CASE("awk prints first field", 0, "left\n", "left right\n",
            "awk", "{print $1}", 0, 0, 0, 0),
    BB_CASE("sha256sum reads file", 0, "hello.txt", 0,
            "sha256sum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("xargs runs echo", 0, "red blue\n", "red blue\n",
            "xargs", "echo", 0, 0, 0, 0),
    BB_CASE("tee mirrors stdin", 0, "tee-line\n", "tee-line\n",
            "tee", 0, 0, 0, 0, 0),
    BB_CASE("sha512sum reads file", 0, "hello.txt", 0,
            "sha512sum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("dd copies one block", 0, "abcd", "abcdef\n",
            "dd", "bs=4", "count=1", 0, 0, 0),
    BB_CASE("cmp accepts identical files", 0, 0, 0,
            "cmp", "hello.txt", "hello.txt", 0, 0, 0),
    BB_CASE("sum reads file", 0, "\n", 0,
            "sum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("od dumps hello bytes", 0, "48 65", 0,
            "od", "-An", "-tx1", "hello.txt", 0, 0),
    BB_CASE("hexdump dumps hello bytes", 0, "48 65 6c", 0,
            "hexdump", "-C", "hello.txt", 0, 0, 0),
    BB_CASE("md5sum reads file", 0, "hello.txt", 0,
            "md5sum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("sha1sum reads file", 0, "hello.txt", 0,
            "sha1sum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("cksum reads file", 0, "hello.txt", 0,
            "cksum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("base64 encodes file", 0, "SGVsbG8", 0,
            "base64", "hello.txt", 0, 0, 0, 0),
    BB_CASE("date emits a line", 0, "\n", 0,
            "date", 0, 0, 0, 0, 0),
    BB_CASE("id reports uid zero", 0, "0", 0,
            "id", "-u", 0, 0, 0, 0),
    BB_CASE("printenv sees PATH", 0, "/bin", 0,
            "printenv", "PATH", 0, 0, 0, 0),
    BB_CASE("env lists PATH", 0, "PATH=/bin", 0,
            "env", 0, 0, 0, 0, 0),
    BB_CASE("ls lists bin", 0, "busybox", 0,
            "ls", "bin", 0, 0, 0, 0),
    BB_CASE("crc32 reads file", 0, "\n", 0,
            "crc32", "hello.txt", 0, 0, 0, 0),
    BB_CASE("[ matches strings", 0, 0, 0,
            "[", "x", "=", "x", "]", 0),
    BB_CASE("[[ matches strings", 0, 0, 0,
            "[[", "x", "=", "x", "]]", 0),
    BB_CASE("arch emits machine", 0, "\n", 0,
            "arch", 0, 0, 0, 0, 0),
    BB_CASE("ascii prints table", 0, "Dec", 0,
            "ascii", 0, 0, 0, 0, 0),
    BB_CASE("ash runs command", 0, "ash-ok\n", 0,
            "ash", "-c", "echo ash-ok", 0, 0, 0),
    BB_CASE("base32 encodes file", 0, "JBSWY3D", 0,
            "base32", "hello.txt", 0, 0, 0, 0),
    BB_CASE("bc adds stdin", 0, "3\n", "1+2\n",
            "bc", 0, 0, 0, 0, 0),
    BB_CASE("cal emits calendar", 0, "\n", 0,
            "cal", 0, 0, 0, 0, 0),
    BB_CASE("clear emits terminal control", 0, "\033[", 0,
            "clear", 0, 0, 0, 0, 0),
    BB_CASE("comm compares identical files", 0, "DUFS", 0,
            "comm", "readme.txt", "readme.txt", 0, 0, 0),
    BB_CASE("chmod accepts file mode", 0, 0, 0,
            "chmod", "644", "hello.txt", 0, 0, 0),
    BB_CASE("chown accepts root owner", 0, 0, 0,
            "chown", "0", "hello.txt", 0, 0, 0),
    BB_CASE("chgrp accepts root group", 0, 0, 0,
            "chgrp", "0", "hello.txt", 0, 0, 0),
    BB_CASE("cp copies file", 0, 0, 0,
            "cp", "hello.txt", "bbcp.txt", 0, 0, 0),
    BB_CASE("dc adds stdin", 0, "3", "1 2 + p\n",
            "dc", 0, 0, 0, 0, 0),
    BB_CASE("df emits header", 0, "Filesystem", 0,
            "df", 0, 0, 0, 0, 0),
    BB_CASE("diff reports file differences", 1, "Hello", 0,
            "diff", "hello.txt", "readme.txt", 0, 0, 0),
    BB_CASE("dos2unix converts stdin", 0, "crlf\n", "crlf\r\n",
            "dos2unix", 0, 0, 0, 0, 0),
    BB_CASE("du sizes file", 0, "hello.txt", 0,
            "du", "-s", "hello.txt", 0, 0, 0),
    BB_CASE("egrep finds file line", 0, "Line two.", 0,
            "egrep", "Line two", "readme.txt", 0, 0, 0),
    BB_CASE("expand expands tab", 0, "a       b\n", "a\tb\n",
            "expand", 0, 0, 0, 0, 0),
    BB_CASE("factor factors number", 0, "12: 2 2 3", 0,
            "factor", "12", 0, 0, 0, 0),
    BB_CASE("fgrep finds file line", 0, "Line two.", 0,
            "fgrep", "Line two", "readme.txt", 0, 0, 0),
    BB_CASE("find locates hello", 0, "hello.txt", 0,
            "find", ".", "-name", "hello.txt", 0, 0),
    BB_CASE("fold wraps stdin", 0, "abc\ndef\n", "abcdef\n",
            "fold", "-w", "3", 0, 0, 0),
    BB_CASE("getopt parses options", 0, "--", 0,
            "getopt", "ab:", "-a", "-b", "x", 0),
    BB_CASE("gzip compresses file", 0, 0, 0,
            "gzip", "-cf", "hello.txt", 0, 0, 0),
    BB_CASE("hd dumps file", 0, "48 65 6c", 0,
            "hd", "hello.txt", 0, 0, 0, 0),
    BB_CASE("hostid emits id", 0, "\n", 0,
            "hostid", 0, 0, 0, 0, 0),
    BB_CASE("hostname emits name", 0, "\n", 0,
            "hostname", 0, 0, 0, 0, 0),
    BB_CASE("hush runs command", 0, "hush-ok\n", 0,
            "hush", "-c", "echo hush-ok", 0, 0, 0),
    BB_CASE("install copies file", 0, 0, 0,
            "install", "hello.txt", "bbinst.txt", 0, 0, 0),
    BB_CASE("ipcalc handles loopback", 0, "NETWORK", 0,
            "ipcalc", "-n", "127.0.0.1", 0, 0, 0),
    BB_CASE("less help is available", 0, "Usage:", 0,
            "less", "--help", 0, 0, 0, 0),
    BB_CASE("lzma help is available", 0, "Usage:", 0,
            "lzma", "--help", 0, 0, 0, 0),
    BB_CASE("mkdir creates directory", 0, 0, 0,
            "mkdir", "bbdir", 0, 0, 0, 0),
    BB_CASE("mktemp creates temp file", 0, "bbtmp.", 0,
            "mktemp", "bbtmp.XXXXXX", 0, 0, 0, 0),
    BB_CASE("more prints file to pipe", 0, "Hello", 0,
            "more", "hello.txt", 0, 0, 0, 0),
    BB_CASE("mv renames file", 0, 0, 0,
            "mv", "bbinst.txt", "bbmv.txt", 0, 0, 0),
    BB_CASE("nice runs command", 0, "nice-ok\n", 0,
            "nice", "echo", "nice-ok", 0, 0, 0),
    BB_CASE("nl numbers file", 0, "1", 0,
            "nl", "readme.txt", 0, 0, 0, 0),
    BB_CASE("nproc emits count", 0, "\n", 0,
            "nproc", 0, 0, 0, 0, 0),
    BB_CASE("paste joins files", 0, "Hello", 0,
            "paste", "hello.txt", "readme.txt", 0, 0, 0),
    BB_CASE("ps lists processes", 0, "PID", 0,
            "ps", 0, 0, 0, 0, 0),
    BB_CASE("pstree returns", 0, 0, 0,
            "pstree", 0, 0, 0, 0, 0),
    BB_CASE("realpath resolves file", 0, "hello.txt", 0,
            "realpath", "hello.txt", 0, 0, 0, 0),
    BB_CASE("rev reverses stdin", 0, "cba\n", "abc\n",
            "rev", 0, 0, 0, 0, 0),
    BB_CASE("rm removes file", 0, 0, 0,
            "rm", "bbmv.txt", 0, 0, 0, 0),
    BB_CASE("rmdir removes directory", 0, 0, 0,
            "rmdir", "bbdir", 0, 0, 0, 0),
    BB_CASE("run-parts tests directory", 0, "bin/", 0,
            "run-parts", "--test", "bin", 0, 0, 0),
    BB_CASE("sha3sum reads file", 0, "hello.txt", 0,
            "sha3sum", "hello.txt", 0, 0, 0, 0),
    BB_CASE("sh runs command", 0, "sh-ok\n", 0,
            "sh", "-c", "echo sh-ok", 0, 0, 0),
    BB_CASE("shuf emits singleton", 0, "1\n", 0,
            "shuf", "-i", "1-1", 0, 0, 0),
    BB_CASE("split writes chunks", 0, 0, 0,
            "split", "-b", "8", "readme.txt", "bbsplit", 0),
    BB_CASE("stat reads file metadata", 0, "hello.txt", 0,
            "stat", "hello.txt", 0, 0, 0, 0),
    BB_CASE("strings reads file", 0, "Hello", 0,
            "strings", "hello.txt", 0, 0, 0, 0),
    BB_CASE("sync returns", 0, 0, 0,
            "sync", 0, 0, 0, 0, 0),
    BB_CASE("tac reverses file lines", 0, "Line three.", 0,
            "tac", "readme.txt", 0, 0, 0, 0),
    BB_CASE("tar writes archive", 0, 0, 0,
            "tar", "-cf", "-", "hello.txt", 0, 0),
    BB_CASE("time runs true", 0, "real", 0,
            "time", "/bin/busybox", "true", 0, 0, 0),
    BB_CASE("timeout runs true", 0, 0, 0,
            "timeout", "1", "/bin/busybox", "true", 0, 0),
    BB_CASE("touch creates file", 0, 0, 0,
            "touch", "bbtouch.txt", 0, 0, 0, 0),
    BB_CASE("tree lists root", 0, "hello.txt", 0,
            "tree", ".", 0, 0, 0, 0),
    BB_CASE("truncate creates file", 0, 0, 0,
            "truncate", "-s", "1", "bbtrunc.txt", 0, 0),
    BB_CASE("ts prefixes stdin", 0, "ts-line", "ts-line\n",
            "ts", 0, 0, 0, 0, 0),
    BB_CASE("tsort sorts graph", 0, "a", "a b\n",
            "tsort", 0, 0, 0, 0, 0),
    BB_CASE("tty reports pipe", 1, "not a tty", 0,
            "tty", 0, 0, 0, 0, 0),
    BB_CASE("unexpand compresses spaces", 0, "x\n", "        x\n",
            "unexpand", 0, 0, 0, 0, 0),
    BB_CASE("unix2dos converts stdin", 0, "lf", "lf\n",
            "unix2dos", 0, 0, 0, 0, 0),
    BB_CASE("unlink removes file", 0, 0, 0,
            "unlink", "bbtouch.txt", 0, 0, 0, 0),
    BB_CASE("usleep zero returns", 0, 0, 0,
            "usleep", "0", 0, 0, 0, 0),
    BB_CASE("uuencode encodes file", 0, "begin", 0,
            "uuencode", "hello.txt", "hello.txt", 0, 0, 0),
    BB_CASE("uudecode decodes stdin", 0, "A", "begin 644 x\n#00``\n`\nend\n",
            "uudecode", "-o", "-", 0, 0, 0),
    BB_CASE("which finds busybox", 0, "/bin/busybox", 0,
            "which", "busybox", 0, 0, 0, 0),
    BB_CASE("xxd dumps file", 0, "4865", 0,
            "xxd", "hello.txt", 0, 0, 0, 0),
    BB_CASE("xz help is available", 0, "Usage:", 0,
            "xz", "--help", 0, 0, 0, 0),
    BB_CASE("acpid help is available", 0, "Usage:", 0,
            "acpid", "--help", 0, 0, 0, 0),
    BB_CASE("addgroup help is available", 0, "Usage:", 0,
            "addgroup", "--help", 0, 0, 0, 0),
    BB_CASE("adduser help is available", 0, "Usage:", 0,
            "adduser", "--help", 0, 0, 0, 0),
    BB_CASE("adjtimex help is available", 0, "Usage:", 0,
            "adjtimex", "--help", 0, 0, 0, 0),
    BB_CASE("arp help is available", 0, "Usage:", 0,
            "arp", "--help", 0, 0, 0, 0),
    BB_CASE("arping help is available", 0, "Usage:", 0,
            "arping", "--help", 0, 0, 0, 0),
    BB_CASE("beep help is available", 0, "Usage:", 0,
            "beep", "--help", 0, 0, 0, 0),
    BB_CASE("blkdiscard help is available", 0, "Usage:", 0,
            "blkdiscard", "--help", 0, 0, 0, 0),
    BB_CASE("blkid help is available", 0, "Usage:", 0,
            "blkid", "--help", 0, 0, 0, 0),
    BB_CASE("blockdev help is available", 0, "Usage:", 0,
            "blockdev", "--help", 0, 0, 0, 0),
    BB_CASE("bootchartd help is available", 0, "Usage:", 0,
            "bootchartd", "--help", 0, 0, 0, 0),
    BB_CASE("brctl help is available", 0, "Usage:", 0,
            "brctl", "--help", 0, 0, 0, 0),
    BB_CASE("chat help is available", 0, "Usage:", 0,
            "chat", "--help", 0, 0, 0, 0),
    BB_CASE("chpasswd help is available", 0, "Usage:", 0,
            "chpasswd", "--help", 0, 0, 0, 0),
    BB_CASE("chpst help is available", 0, "Usage:", 0,
            "chpst", "--help", 0, 0, 0, 0),
    BB_CASE("chroot help is available", 0, "Usage:", 0,
            "chroot", "--help", 0, 0, 0, 0),
    BB_CASE("chrt help is available", 0, "Usage:", 0,
            "chrt", "--help", 0, 0, 0, 0),
    BB_CASE("chvt help is available", 0, "Usage:", 0,
            "chvt", "--help", 0, 0, 0, 0),
    BB_CASE("conspy help is available", 0, "Usage:", 0,
            "conspy", "--help", 0, 0, 0, 0),
    BB_CASE("cttyhack help is available", 0, "Usage:", 0,
            "cttyhack", "--help", 0, 0, 0, 0),
    BB_CASE("deallocvt help is available", 0, "Usage:", 0,
            "deallocvt", "--help", 0, 0, 0, 0),
    BB_CASE("devmem help is available", 0, "Usage:", 0,
            "devmem", "--help", 0, 0, 0, 0),
    BB_CASE("dhcprelay help is available", 0, "Usage:", 0,
            "dhcprelay", "--help", 0, 0, 0, 0),
    BB_CASE("dnsd help is available", 0, "Usage:", 0,
            "dnsd", "--help", 0, 0, 0, 0),
    BB_CASE("add-shell help is available", 0, "Usage:", 0,
            "add-shell", "--help", 0, 0, 0, 0),
    BB_CASE("bunzip2 help is available", 0, "Usage:", 0,
            "bunzip2", "--help", 0, 0, 0, 0),
    BB_CASE("bzcat help is available", 0, "Usage:", 0,
            "bzcat", "--help", 0, 0, 0, 0),
    BB_CASE("bzip2 help is available", 0, "Usage:", 0,
            "bzip2", "--help", 0, 0, 0, 0),
    BB_CASE("chattr help is available", 0, "Usage:", 0,
            "chattr", "--help", 0, 0, 0, 0),
    BB_CASE("cpio help is available", 0, "Usage:", 0,
            "cpio", "--help", 0, 0, 0, 0),
    BB_CASE("crond help is available", 0, "Usage:", 0,
            "crond", "--help", 0, 0, 0, 0),
    BB_CASE("crontab help is available", 0, "Usage:", 0,
            "crontab", "--help", 0, 0, 0, 0),
    BB_CASE("cryptpw help is available", 0, "Usage:", 0,
            "cryptpw", "--help", 0, 0, 0, 0),
    BB_CASE("delgroup help is available", 0, "Usage:", 0,
            "delgroup", "--help", 0, 0, 0, 0),
    BB_CASE("deluser help is available", 0, "Usage:", 0,
            "deluser", "--help", 0, 0, 0, 0),
    BB_CASE("depmod help is available", 0, "Usage:", 0,
            "depmod", "--help", 0, 0, 0, 0),
    BB_CASE("dnsdomainname help is available", 0, "No help available", 0,
            "dnsdomainname", "--help", 0, 0, 0, 0),
    BB_CASE("dpkg help is available", 0, "Usage:", 0,
            "dpkg", "--help", 0, 0, 0, 0),
    BB_CASE("dpkg-deb help is available", 0, "Usage:", 0,
            "dpkg-deb", "--help", 0, 0, 0, 0),
    BB_CASE("dumpkmap help is available", 0, "Usage:", 0,
            "dumpkmap", "--help", 0, 0, 0, 0),
    BB_CASE("dumpleases help is available", 0, "Usage:", 0,
            "dumpleases", "--help", 0, 0, 0, 0),
    BB_CASE("eject help is available", 0, "Usage:", 0,
            "eject", "--help", 0, 0, 0, 0),
    BB_CASE("envdir help is available", 0, "Usage:", 0,
            "envdir", "--help", 0, 0, 0, 0),
    BB_CASE("envuidgid help is available", 0, "Usage:", 0,
            "envuidgid", "--help", 0, 0, 0, 0),
    BB_CASE("ether-wake help is available", 0, "Usage:", 0,
            "ether-wake", "--help", 0, 0, 0, 0),
    BB_CASE("fakeidentd help is available", 0, "Usage:", 0,
            "fakeidentd", "--help", 0, 0, 0, 0),
    BB_CASE("fallocate help is available", 0, "Usage:", 0,
            "fallocate", "--help", 0, 0, 0, 0),
    BB_CASE("fatattr help is available", 0, "Usage:", 0,
            "fatattr", "--help", 0, 0, 0, 0),
    BB_CASE("fbset help is available", 0, "Usage:", 0,
            "fbset", "--help", 0, 0, 0, 0),
    BB_CASE("fbsplash help is available", 0, "Usage:", 0,
            "fbsplash", "--help", 0, 0, 0, 0),
    BB_CASE("fdflush help is available", 0, "Usage:", 0,
            "fdflush", "--help", 0, 0, 0, 0),
    BB_CASE("fdformat help is available", 0, "Usage:", 0,
            "fdformat", "--help", 0, 0, 0, 0),
    BB_CASE("fdisk help is available", 0, "Usage:", 0,
            "fdisk", "--help", 0, 0, 0, 0),
    BB_CASE("fgconsole help is available", 0, "Usage:", 0,
            "fgconsole", "--help", 0, 0, 0, 0),
    BB_CASE("findfs help is available", 0, "Usage:", 0,
            "findfs", "--help", 0, 0, 0, 0),
    BB_CASE("flock help is available", 0, "Usage:", 0,
            "flock", "--help", 0, 0, 0, 0),
    BB_CASE("free help is available", 0, "Usage:", 0,
            "free", "--help", 0, 0, 0, 0),
    BB_CASE("freeramdisk help is available", 0, "Usage:", 0,
            "freeramdisk", "--help", 0, 0, 0, 0),
    BB_CASE("fsck help is available", 0, "Usage:", 0,
            "fsck", "--help", 0, 0, 0, 0),
    BB_CASE("fsck.minix help is available", 0, "Usage:", 0,
            "fsck.minix", "--help", 0, 0, 0, 0),
    BB_CASE("fsfreeze help is available", 0, "Usage:", 0,
            "fsfreeze", "--help", 0, 0, 0, 0),
    BB_CASE("fstrim help is available", 0, "Usage:", 0,
            "fstrim", "--help", 0, 0, 0, 0),
    BB_CASE("fsync help is available", 0, "Usage:", 0,
            "fsync", "--help", 0, 0, 0, 0),
    BB_CASE("ftpd help is available", 0, "Usage:", 0,
            "ftpd", "--help", 0, 0, 0, 0),
    BB_CASE("ftpget help is available", 0, "Usage:", 0,
            "ftpget", "--help", 0, 0, 0, 0),
    BB_CASE("ftpput help is available", 0, "Usage:", 0,
            "ftpput", "--help", 0, 0, 0, 0),
    BB_CASE("fuser help is available", 0, "Usage:", 0,
            "fuser", "--help", 0, 0, 0, 0),
    BB_CASE("getty help is available", 0, "Usage:", 0,
            "getty", "--help", 0, 0, 0, 0),
    BB_CASE("groups help is available", 0, "Usage:", 0,
            "groups", "--help", 0, 0, 0, 0),
    BB_CASE("gunzip help is available", 0, "Usage:", 0,
            "gunzip", "--help", 0, 0, 0, 0),
    BB_CASE("halt help is available", 0, "Usage:", 0,
            "halt", "--help", 0, 0, 0, 0),
    BB_CASE("hdparm help is available", 0, "Usage:", 0,
            "hdparm", "--help", 0, 0, 0, 0),
    BB_CASE("hexedit help is available", 0, "Usage:", 0,
            "hexedit", "--help", 0, 0, 0, 0),
    BB_CASE("httpd help is available", 0, "Usage:", 0,
            "httpd", "--help", 0, 0, 0, 0),
    BB_CASE("hwclock help is available", 0, "Usage:", 0,
            "hwclock", "--help", 0, 0, 0, 0),
    BB_CASE("i2cdetect help is available", 0, "Usage:", 0,
            "i2cdetect", "--help", 0, 0, 0, 0),
    BB_CASE("i2cdump help is available", 0, "Usage:", 0,
            "i2cdump", "--help", 0, 0, 0, 0),
    BB_CASE("i2cget help is available", 0, "Usage:", 0,
            "i2cget", "--help", 0, 0, 0, 0),
    BB_CASE("i2cset help is available", 0, "Usage:", 0,
            "i2cset", "--help", 0, 0, 0, 0),
    BB_CASE("i2ctransfer help is available", 0, "Usage:", 0,
            "i2ctransfer", "--help", 0, 0, 0, 0),
    BB_CASE("ifconfig help is available", 0, "Usage:", 0,
            "ifconfig", "--help", 0, 0, 0, 0),
    BB_CASE("ifdown help is available", 0, "Usage:", 0,
            "ifdown", "--help", 0, 0, 0, 0),
    BB_CASE("ifenslave help is available", 0, "Usage:", 0,
            "ifenslave", "--help", 0, 0, 0, 0),
    BB_CASE("ifplugd help is available", 0, "Usage:", 0,
            "ifplugd", "--help", 0, 0, 0, 0),
    BB_CASE("ifup help is available", 0, "Usage:", 0,
            "ifup", "--help", 0, 0, 0, 0),
    BB_CASE("inetd help is available", 0, "Usage:", 0,
            "inetd", "--help", 0, 0, 0, 0),
    BB_CASE("init help is available", 0, "Usage:", 0,
            "init", "--help", 0, 0, 0, 0),
    BB_CASE("insmod help is available", 0, "Usage:", 0,
            "insmod", "--help", 0, 0, 0, 0),
    BB_CASE("ionice help is available", 0, "Usage:", 0,
            "ionice", "--help", 0, 0, 0, 0),
    BB_CASE("iostat help is available", 0, "Usage:", 0,
            "iostat", "--help", 0, 0, 0, 0),
    BB_CASE("ipaddr help is available", 0, "Usage:", 0,
            "ipaddr", "--help", 0, 0, 0, 0),
    BB_CASE("ipcrm help is available", 0, "Usage:", 0,
            "ipcrm", "--help", 0, 0, 0, 0),
    BB_CASE("ipcs help is available", 0, "Usage:", 0,
            "ipcs", "--help", 0, 0, 0, 0),
    BB_CASE("iplink help is available", 0, "Usage:", 0,
            "iplink", "--help", 0, 0, 0, 0),
    BB_CASE("ipneigh help is available", 0, "Usage:", 0,
            "ipneigh", "--help", 0, 0, 0, 0),
    BB_CASE("iproute help is available", 0, "Usage:", 0,
            "iproute", "--help", 0, 0, 0, 0),
    BB_CASE("iprule help is available", 0, "Usage:", 0,
            "iprule", "--help", 0, 0, 0, 0),
    BB_CASE("iptunnel help is available", 0, "Usage:", 0,
            "iptunnel", "--help", 0, 0, 0, 0),
    BB_CASE("kbd_mode help is available", 0, "Usage:", 0,
            "kbd_mode", "--help", 0, 0, 0, 0),
    BB_CASE("killall help is available", 0, "Usage:", 0,
            "killall", "--help", 0, 0, 0, 0),
    BB_CASE("killall5 help is available", 0, "Usage:", 0,
            "killall5", "--help", 0, 0, 0, 0),
    BB_CASE("klogd help is available", 0, "Usage:", 0,
            "klogd", "--help", 0, 0, 0, 0),
    BB_CASE("last help is available", 0, "Usage:", 0,
            "last", "--help", 0, 0, 0, 0),
    BB_CASE("link help is available", 0, "Usage:", 0,
            "link", "--help", 0, 0, 0, 0),
    BB_CASE("linux32 help is available", 0, "No help available", 0,
            "linux32", "--help", 0, 0, 0, 0),
    BB_CASE("linux64 help is available", 0, "No help available", 0,
            "linux64", "--help", 0, 0, 0, 0),
    BB_CASE("loadfont help is available", 0, "Usage:", 0,
            "loadfont", "--help", 0, 0, 0, 0),
    BB_CASE("loadkmap help is available", 0, "Usage:", 0,
            "loadkmap", "--help", 0, 0, 0, 0),
    BB_CASE("logger help is available", 0, "Usage:", 0,
            "logger", "--help", 0, 0, 0, 0),
    BB_CASE("login help is available", 0, "Usage:", 0,
            "login", "--help", 0, 0, 0, 0),
    BB_CASE("logname help is available", 0, "Usage:", 0,
            "logname", "--help", 0, 0, 0, 0),
    BB_CASE("logread help is available", 0, "Usage:", 0,
            "logread", "--help", 0, 0, 0, 0),
    BB_CASE("losetup help is available", 0, "Usage:", 0,
            "losetup", "--help", 0, 0, 0, 0),
    BB_CASE("lsattr help is available", 0, "Usage:", 0,
            "lsattr", "--help", 0, 0, 0, 0),
    BB_CASE("lsmod help is available", 0, "Usage:", 0,
            "lsmod", "--help", 0, 0, 0, 0),
    BB_CASE("lsof help is available", 0, "Usage:", 0,
            "lsof", "--help", 0, 0, 0, 0),
    BB_CASE("lspci help is available", 0, "Usage:", 0,
            "lspci", "--help", 0, 0, 0, 0),
    BB_CASE("lsscsi help is available", 0, "No help available", 0,
            "lsscsi", "--help", 0, 0, 0, 0),
    BB_CASE("lsusb help is available", 0, "No help available", 0,
            "lsusb", "--help", 0, 0, 0, 0),
    BB_CASE("lzcat help is available", 0, "Usage:", 0,
            "lzcat", "--help", 0, 0, 0, 0),
    BB_CASE("lzop help is available", 0, "Usage:", 0,
            "lzop", "--help", 0, 0, 0, 0),
    BB_CASE("makedevs help is available", 0, "Usage:", 0,
            "makedevs", "--help", 0, 0, 0, 0),
    BB_CASE("makemime help is available", 0, "Usage:", 0,
            "makemime", "--help", 0, 0, 0, 0),
    BB_CASE("mdev help is available", 0, "Usage:", 0,
            "mdev", "--help", 0, 0, 0, 0),
};

static int log_fd = -1;

static int text_contains(const char *haystack, const char *needle)
{
    int hlen;
    int nlen;

    if (!needle || needle[0] == '\0')
        return 1;
    if (!haystack)
        return 0;

    hlen = (int)strlen(haystack);
    nlen = (int)strlen(needle);
    if (nlen > hlen)
        return 0;

    for (int i = 0; i <= hlen - nlen; i++) {
        if (strncmp(haystack + i, needle, (unsigned int)nlen) == 0)
            return 1;
    }
    return 0;
}

static void emit(const char *s)
{
    int len = (int)strlen(s);

    sys_fwrite(1, s, len);
    if (log_fd >= 0)
        sys_fwrite(log_fd, s, len);
}

static void emitf(const char *fmt, const char *name, int a, int b)
{
    char buf[256];

    snprintf(buf, sizeof(buf), fmt, name, a, b);
    emit(buf);
}

static int wait_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status))
        return 128 + WSTOPSIG(status);
    return 255;
}

static int run_case(const bb_case_t *tc, char *out, int out_cap)
{
    int out_pipe[2];
    int in_pipe[2];
    int have_input = tc->stdin_text != 0;
    int pid;
    int used = 0;
    char *argv[9];
    char *envp[] = { "PATH=/bin", 0 };

    if (sys_pipe(out_pipe) != 0)
        return 255;
    if (have_input && sys_pipe(in_pipe) != 0) {
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);
        return 255;
    }

    pid = sys_fork();
    if (pid == 0) {
        sys_dup2(out_pipe[1], 1);
        sys_dup2(out_pipe[1], 2);
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);

        if (have_input) {
            sys_dup2(in_pipe[0], 0);
            sys_close(in_pipe[0]);
            sys_close(in_pipe[1]);
        }

        argv[0] = "busybox";
        for (int i = 0; i < 7; i++)
            argv[i + 1] = (char *)tc->argv[i];
        sys_execve("/bin/busybox", argv, envp);
        sys_write("exec /bin/busybox failed\n");
        sys_exit(127);
    }

    if (pid < 0) {
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);
        if (have_input) {
            sys_close(in_pipe[0]);
            sys_close(in_pipe[1]);
        }
        return 255;
    }

    sys_close(out_pipe[1]);
    if (have_input) {
        sys_close(in_pipe[0]);
        sys_fwrite(in_pipe[1], tc->stdin_text, (int)strlen(tc->stdin_text));
        sys_close(in_pipe[1]);
    }

    for (;;) {
        char chunk[128];
        int n = sys_read(out_pipe[0], chunk, sizeof(chunk));
        if (n <= 0)
            break;
        if (used < out_cap - 1) {
            int room = out_cap - 1 - used;
            int copy = n < room ? n : room;
            memcpy(out + used, chunk, (unsigned int)copy);
            used += copy;
        }
    }
    out[used] = '\0';
    sys_close(out_pipe[0]);

    return wait_exit_code(sys_waitpid(pid, 0));
}

int main(void)
{
    int passed = 0;

    log_fd = sys_create("bbcompat.log");
    emit("BBCOMPAT BEGIN\n");

    for (int i = 0; i < BBCOMPAT_TOTAL; i++) {
        char out[BB_OUT_CAP];
        int code = run_case(&cases[i], out, sizeof(out));
        int ok = code == cases[i].expected_exit &&
                 text_contains(out, cases[i].must_contain);

        if (ok) {
            passed++;
            emitf("BBCOMPAT PASS %s exit=%d/%d\n",
                  cases[i].name, code, cases[i].expected_exit);
        } else {
            emitf("BBCOMPAT FAIL %s exit=%d/%d\n",
                  cases[i].name, code, cases[i].expected_exit);
            if (cases[i].must_contain)
                emit(cases[i].must_contain);
            emit("BBCOMPAT OUTPUT BEGIN\n");
            emit(out);
            emit("BBCOMPAT OUTPUT END\n");
        }
    }

    emitf("BBCOMPAT SUMMARY %s %d/%d\n", "passed", passed, BBCOMPAT_TOTAL);
    emit("BBCOMPAT DONE\n");
    if (log_fd >= 0)
        sys_close(log_fd);

    return passed == BBCOMPAT_TOTAL ? 0 : 1;
}
