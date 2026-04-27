#include "stdio.h"
#include "stdlib.h"
#include <stdint.h>

int main(int argc, char **argv)
{
	char *overflow = (char *)malloc((size_t)UINT32_MAX);
	if (overflow) {
		printf("CHELLO MALLOC FAIL overflow allocation returned pointer\n");
		return 1;
	}

	char *big = (char *)malloc(256 * 1024);
	if (!big) {
		printf("CHELLO MALLOC FAIL large allocation returned null\n");
		return 1;
	}

	big[0] = 'a';
	big[(256 * 1024) - 1] = 'z';
	if (big[0] != 'a' || big[(256 * 1024) - 1] != 'z') {
		printf("CHELLO MALLOC FAIL large allocation contents\n");
		free(big);
		return 1;
	}
	free(big);

	printf("Hello from C userland!\n");
	printf("argc=%d\n", argc);
	if (argc > 0 && argv && argv[0])
		printf("argv[0]=%s\n", argv[0]);
	return 0;
}
