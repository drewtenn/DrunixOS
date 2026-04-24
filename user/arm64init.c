/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "syscall_arm64.h"

int main(void)
{
	arm64_sys_write(1, "ARM64 init: entered\n", 20);
	arm64_sys_write(1, "ARM64 init: pass\n", 17);
	return 0;
}
