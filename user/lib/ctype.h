/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef CTYPE_H
#define CTYPE_H

/*
 * ctype.h — ASCII character classification and case conversion.
 *
 * Every function takes and returns an int so callers can pass either a
 * signed char or the special value EOF (-1) without undefined behaviour.
 * Values outside the 7-bit ASCII range always return 0 (or the input
 * unchanged, for the conversion functions).
 */

#ifdef __cplusplus
extern "C" {
#endif

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);

int toupper(int c);
int tolower(int c);

#ifdef __cplusplus
}
#endif

#endif
