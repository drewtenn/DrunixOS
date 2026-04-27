/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * printenv.c — user-space environment lookup utility.
 */

#include "stdio.h"
#include "stdlib.h"
#include "syscall.h"

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc == 1)
    {
        if (!environ)
            return 0;
        for (int i = 0; environ[i]; i++)
            printf("%s\n", environ[i]);
        return 0;
    }

    for (int i = 1; i < argc; i++)
    {
        char *value = getenv(argv[i]);
        if (value)
            printf("%s\n", value);
        else
            rc = 1;
    }

    return rc;
}
