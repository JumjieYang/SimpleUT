#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <ucontext.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "./src/sut.h"
#include "./src/queue.h"

/*
 *	This implementation supports multiple I/O operations concurrently
 *	I/O requests are placed in the I/O queue, processed in the FIFO manner
 *	Server response in sut_read() is stored in a message queue
 *  Task that requests read operation would retrieve their corresponding response from the queue when C-EXEC runs it again
 */ 

#define THREAD_STACK_SIZE 1024 * 64
#define MAX_THREADS 15
#define BUF_SIZE 256
 
void iexec(void);
void cexec(void);
int connect_to_server(int *sockfd, char *dest, int port);

typedef struct __threaddesc
{
	int sockfd;
	bool is_connected;
	char *buf;
	ucontext_t threadcontext;
} threaddesc;

// signal information to I-EXEC
typedef struct __request
{
	int op, port, size;
	char *dest, *buf;
	threaddesc *thread;
} request;

bool sd;	// shutdown 
int numthreads;
int i_num, c_num;	// size of ready queue and wait queue

struct queue i_queue;	// ready queue
struct queue c_queue;	// wait queue
struct queue msg_queue;	// message queue, to store the server response in sut_read()
pthread_t *I_EXEC;
pthread_t *C_EXEC;
threaddesc *c_cur;	// current running thread of C_EXEC
request *i_cur;	// current running thread of I_EXEC
ucontext_t *parent;	// context of C-EXEC
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;;

void sut_init()
{
	// initialize variables and executors 
	sd = false;
	i_cur = NULL;
	c_cur = NULL;
	i_num = 0;
	c_num = 0;
	numthreads = 0;

	c_queue = queue_create();
	queue_init(&c_queue); 
	i_queue = queue_create();
	queue_init(&i_queue); 
	msg_queue = queue_create();
	queue_init(&msg_queue);

	parent = (ucontext_t *)malloc(sizeof(ucontext_t));
	I_EXEC = (pthread_t *)malloc(sizeof(pthread_t));
	C_EXEC = (pthread_t *)malloc(sizeof(pthread_t));

	pthread_create(C_EXEC, NULL, (void *)cexec, 0);
	pthread_create(I_EXEC, NULL, (void *)iexec, 0);
}

// create a new task and insert into ready queue
bool sut_create(sut_task_f fn)
{
	threaddesc *tdescptr;

	if (numthreads >= 15)
	{
		printf("FATAL: Maximum thread limit reached... creation failed! \n");
		return false;
	}

	threaddesc *uthread = (threaddesc *)malloc(sizeof(threaddesc));
	getcontext(&(uthread->threadcontext));
	uthread->threadcontext.uc_stack.ss_sp = (char *)malloc(THREAD_STACK_SIZE);
	uthread->threadcontext.uc_stack.ss_size = THREAD_STACK_SIZE;
	uthread->threadcontext.uc_link = 0;
	uthread->threadcontext.uc_stack.ss_flags = 0;
	uthread->sockfd = 0;
	uthread->is_connected = false;
	makecontext(&(uthread->threadcontext), fn, 0);
	numthreads++;

	// insert the task into the ready queue
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&c_queue, queue_new_node((void *)uthread));
	c_num ++;
	pthread_mutex_unlock(&mutex);

	return true;
}

void sut_yield()
{
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&c_queue, queue_new_node((void *)c_cur));
	c_num ++;
	pthread_mutex_unlock(&mutex);
	swapcontext(&(c_cur->threadcontext), parent);
}

void sut_exit()
{
	// exit the task
	free(c_cur);
	setcontext(parent);
}

void sut_open(char *dest, int port)
{
	request *req = (request *)malloc(sizeof(request));
	req->dest = dest;
	req->port = port;
	req->op = 0;
	req->thread = c_cur;

	// enqueue the request
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&i_queue, queue_new_node((void *)req));
	i_num ++;
	pthread_mutex_unlock(&mutex);

	// yield control to c-exec
	swapcontext(&(c_cur->threadcontext), parent);
}

void sut_write(char *buf, int size)
{
	request *req = (request *)malloc(sizeof(request));
	req->size = size;
	req->buf = buf;
	req->op = 1;
	req->thread = c_cur;

	// enqueue the request
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&i_queue, queue_new_node((void *)req));
	i_num ++;
	pthread_mutex_unlock(&mutex);
}

// close the socket of current running task
void sut_close()
{
	close(c_cur->sockfd);
	c_cur->is_connected = false;
}

char *sut_read()
{
	request *req = (request *)malloc(sizeof(request));
	req->op = 2;
	req->thread = c_cur;

	// enqueue the request
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&i_queue, queue_new_node((void *)req));
	i_num ++;
	pthread_mutex_unlock(&mutex);

	// yield the control to c-exec
	swapcontext(&(c_cur->threadcontext), parent);

	// retrieve the response
	struct queue_entry *entry = queue_pop_head(&msg_queue);
	return entry->data;
}

void sut_shutdown()
{
	sd = true;
	pthread_join(*I_EXEC, NULL);
	pthread_join(*C_EXEC, NULL);
	free(I_EXEC);
	free(C_EXEC);
	free(parent);
}

// IO executor
void iexec()
{
	while (true)
	{
		// busy wait
		while (i_num == 0)
		{
			// exit if no task is left in both queues, C-exec is not running, and shutdown is true
			if (c_num == 0 && c_cur == NULL && sd == true)
			{
				return;
			}
			usleep(100);
		}

		// dequeue
		pthread_mutex_lock(&mutex);
		struct queue_entry *entry = queue_pop_head(&i_queue);
		i_num --;
		pthread_mutex_unlock(&mutex);
		i_cur = entry->data;
		
		// check the operation requested
		// op: open a connection
		if (i_cur->op == 0)
		{
			connect_to_server(&(i_cur->thread->sockfd), i_cur->dest, i_cur->port);
			i_cur->thread->is_connected = true;

			// reschedule the task to c-exec
			pthread_mutex_lock(&mutex);
			queue_insert_tail(&c_queue, queue_new_node((void *)(i_cur->thread)));
			c_num ++;
			pthread_mutex_unlock(&mutex);
		}
		// op: write to the server
		else if (i_cur->op == 1)
		{
			if (i_cur->thread->is_connected == false)
			{
				printf("FATAL: This task is not connected to any server!\n");
			}
			else
			{
				send(i_cur->thread->sockfd, i_cur->buf, i_cur->size, 0);
			}
		}
		// op: read from the server
		else if (i_cur->op == 2)
		{
			char* msg = (char*)malloc(BUF_SIZE*sizeof(char));
			memset(msg, 0, BUF_SIZE);
			if (i_cur->thread->is_connected == false)
			{
				printf("FATAL: This task is not connected to any server!\n");
				queue_insert_tail(&msg_queue, queue_new_node((void *)msg));
			}
			else
			{
				recv(i_cur->thread->sockfd, msg, BUF_SIZE, 0);
				// store the response in the message queue for the thread to retrieve
				queue_insert_tail(&msg_queue, queue_new_node((void *)msg));
			}
			
			// reschedule the task to c-exec
			pthread_mutex_lock(&mutex);
			queue_insert_tail(&c_queue, queue_new_node((void *)(i_cur->thread)));
			c_num ++;
			pthread_mutex_unlock(&mutex);
		}
		free(i_cur);
		i_cur = NULL;
	}
}

// C-executor
void cexec()
{
	while (true)
	{
		while (c_num == 0)
		{
			// exit if no task is left in both queues, I-exec is not running, and shutdown is true
			if (i_num == 0 && i_cur == NULL && sd == true)
			{
				return;
			}
			usleep(100);
		}
		// dequeue
		pthread_mutex_lock(&mutex);
		struct queue_entry *entry = queue_pop_head(&c_queue);
		c_num --;
		pthread_mutex_unlock(&mutex);
		c_cur = entry->data;

		// run the task
		swapcontext(parent, &(c_cur->threadcontext));
		c_cur = NULL;
	}
}

// from a1_lib.c
int connect_to_server(int *sockfd, char *dest, int port)
{
	struct sockaddr_in server_address = {0};
	*sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		perror("Failed to create a new socket\n");
		return -1;
	}

	server_address.sin_family = AF_INET;
	inet_pton(AF_INET, dest, &(server_address.sin_addr.s_addr));
	server_address.sin_port = htons(port);
	if (connect(*sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
	{
		perror("Failed to connect to server\n");
		return -1;
	}
	return 0;
}
