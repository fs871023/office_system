#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <linux/sched.h>

void *runner1(void)
{
	while(1)
	{
		printf("0\n");
		double a = 0;
                // waste enough cpu time
		for (int i = 0; i < 10000000; i++)	a += 0.1f;
	}
}


void *runner2(void)
{
	while(1)
	{
		printf("1\n");
		double a = 0;
                // waste enough cpu time
		for (int i = 0; i < 10000000; i++)	a += 0.1f;
	}
}


void *runner3(void)
{
	while(1)
	{
		printf("2\n");
		double a = 0;
                // waste enough cpu time
		for (int i = 0; i < 10000000; i++)	a += 0.1f;
	}
}


int main(int argc, char *argv[])
{
	pthread_t tid[3];
	pthread_attr_t attrs[3];
	struct sched_param param[3];
    
        for(int i=0; i<3; i++){
            pthread_attr_init(&attrs[i]);
            pthread_attr_setschedpolicy(&attrs[i], SCHED_RR);
            pthread_attr_setinheritsched(&attrs[i], PTHREAD_EXPLICIT_SCHED);
            param[i].sched_priority = 20;
            pthread_attr_setschedparam(&attrs[i], &param[i]);
        }
        
        pthread_create(&tid[0], &attrs[0], runner1, NULL);
        pthread_create(&tid[1], &attrs[1], runner2, NULL);
        pthread_create(&tid[2], &attrs[2], runner3, NULL);
        
        pthread_join(tid[0], NULL);
        pthread_join(tid[1], NULL);
        pthread_join(tid[2], NULL);
        
        pthread_exit(NULL);
        return 0;
}
