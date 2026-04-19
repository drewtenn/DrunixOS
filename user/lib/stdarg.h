/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef STDARG_H
#define STDARG_H

typedef char *va_list;

#define __va_round(type) \
    (((sizeof(type) + sizeof(int) - 1) / sizeof(int)) * sizeof(int))

#define va_start(ap, last) ((ap) = (char *)&(last) + __va_round(last))
#define va_arg(ap, type) \
    ((ap) += __va_round(type), *(type *)((ap) - __va_round(type)))
#define va_end(ap) ((void)0)
#define va_copy(dst, src) ((dst) = (src))

#endif
