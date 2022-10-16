#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main (void)
{
	int a = 6;
	int b = 6;
	int res = 0;
	syscall(439, 0, a, b, &res);
	printf("6 + 6 = %d\n", res);

	a=1,b=0;
	syscall(439, 0, b, a, &res);
	printf("1 + 0 = %d\n", res);

	a=1,b=5;	
	syscall(439, 1, a, b, &res);
	printf("1 - 5 = %d\n", res);

	a=0,b=15;
	syscall(439, 0, b, a, &res);
	printf("0 + (-15) = -%d\n", res);

	a=55,b=5;
	syscall(439, 0, b, a, &res);
	printf("(-55) + (-5) = -%d\n", res);
	
	return 0;
}
	
