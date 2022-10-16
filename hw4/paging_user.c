#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <string.h>

#define CHR_DEV_NAME "/dev/myChrDevice"
#define MY_IOCTL_QUIRK 'c'
#define CMD_WRITE_REQUST   		_IOW(MY_IOCTL_QUIRK, 1, char)
#define CMD_PRINT_FREE_LIST 		_IOR(MY_IOCTL_QUIRK, 2, char)
#define CMD_PRINT_RECLAIM_LIST 	_IOR(MY_IOCTL_QUIRK, 3, char)

int main(int argc, char *argv[])
{
	if(argc != 2)
	{
		printf("Please add dataset file path.\n");
		return -1;
	}

	FILE *fp = fopen(argv[1], "r");
	if(!fp) {
		perror("Open dataset failed.\n");
		return -1;
	}

	int fd;
	if((fd = open(CHR_DEV_NAME, O_RDWR)) < 0){
		perror("Open module failed.\n");
		return -1;
	}

	char tmp[10];
	char *line = NULL;
	ssize_t read;
	size_t len = 0;
	char *endptr;

	while( fscanf(fp, "%9s", tmp) != EOF)
	{
		unsigned long request = strtoll(tmp, &endptr, 10);
		printf("Now send request:%ld\n", request);
		ioctl(fd, CMD_WRITE_REQUST, request);
	}
	ioctl(fd, CMD_PRINT_FREE_LIST, 0);
	ioctl(fd, CMD_PRINT_RECLAIM_LIST, 0);

	fclose(fp);
	close(fd);
	return 0;
}

