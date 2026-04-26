/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * module_exports.c — exported kernel symbol table for loadable modules.
 */

#include "module.h"
#include "kheap.h"
#include <stddef.h>
#include "klog.h"
#include "blkdev.h"
#include "chardev.h"
#include "irq.h"
#include "vfs.h"

/*
 * kernel_exports: the compile-time table of kernel symbols that loadable
 * modules may reference.  Each module's undefined externals are resolved
 * against this table before module_init is called.
 *
 * To expose a new kernel function to modules, add an entry here.
 */
const ksym_t kernel_exports[] = {
    {"kmalloc", (void *)kmalloc},
    {"kfree", (void *)kfree},
    {"klog", (void *)klog},
    {"klog_uint", (void *)klog_uint},
    {"klog_hex", (void *)klog_hex},
    {"blkdev_register", (void *)blkdev_register},
    {"blkdev_get", (void *)blkdev_get},
    {"chardev_register", (void *)chardev_register},
    {"chardev_get", (void *)chardev_get},
    {"irq_register", (void *)irq_register},
    {"vfs_register", (void *)vfs_register},
    {NULL, NULL} /* sentinel */
};

const uint32_t kernel_exports_count =
    sizeof(kernel_exports) / sizeof(kernel_exports[0]) - 1u;
