/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * chardev.c — character device registry and dispatch helpers.
 */

#include "chardev.h"

static struct {
	char name[CHARDEV_NAME_MAX];
	const chardev_ops_t *ops;
} chardev_table[CHARDEV_MAX];

int chardev_register(const char *name, const chardev_ops_t *ops)
{
	for (int i = 0; i < CHARDEV_MAX; i++) {
		if (chardev_table[i].name[0] == '\0') {
			int j = 0;
			while (j < CHARDEV_NAME_MAX - 1 && name[j]) {
				chardev_table[i].name[j] = name[j];
				j++;
			}
			chardev_table[i].name[j] = '\0';
			chardev_table[i].ops = ops;
			return 0;
		}
	}
	return -1;
}

const chardev_ops_t *chardev_get(const char *name)
{
	for (int i = 0; i < CHARDEV_MAX; i++) {
		if (chardev_table[i].name[0] == '\0')
			continue;
		int j = 0;
		while (j < CHARDEV_NAME_MAX - 1 && name[j] &&
		       chardev_table[i].name[j] == name[j])
			j++;
		if (chardev_table[i].name[j] == '\0' && name[j] == '\0')
			return chardev_table[i].ops;
	}
	return 0;
}
