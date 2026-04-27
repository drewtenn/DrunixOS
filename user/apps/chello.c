#include "stdio.h"

int main(int argc, char **argv)
{
	printf("Hello from C userland!\n");
	printf("argc=%d\n", argc);
	if (argc > 0 && argv && argv[0])
		printf("argv[0]=%s\n", argv[0]);
	return 0;
}
