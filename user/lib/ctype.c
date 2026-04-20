/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * ctype.c — ASCII character classification.
 *
 * Keeping these out of line in a .c file rather than inlining as macros
 * in the header avoids the classic multiple-evaluation traps of the C89
 * macro implementations and is plenty fast for anything this project does.
 */

#include "ctype.h"

int isdigit(int c)
{
	return (c >= '0' && c <= '9');
}

int isalpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c)
{
	return isdigit(c) || isalpha(c);
}

int isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
	       c == '\f';
}

int isupper(int c)
{
	return c >= 'A' && c <= 'Z';
}

int islower(int c)
{
	return c >= 'a' && c <= 'z';
}

int toupper(int c)
{
	if (islower(c))
		return c - ('a' - 'A');
	return c;
}

int tolower(int c)
{
	if (isupper(c))
		return c + ('a' - 'A');
	return c;
}
