#include<linux/syscalls.h>
#include<linux/kernel.h>
#include<asm/uaccess.h>
SYSCALL_DEFINE4(mycall, int, operator, int, a, int, b, int  __user*, res)
{
	int ans = 0;
	

//sick my duck

	if(operator == 0){
		ans = a + b;
		if((copy_to_user(res, &ans, sizeof(ans) )) );
	}
	if(operator == 1){
		ans = a - b;
		if((copy_to_user(res, &ans, sizeof(ans) )) );
	}

	return 0;
}
