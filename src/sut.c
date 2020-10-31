#include <stdbool.h>
#include "sut.h"
#include "queue.h"
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include "socket.c"
#include <string.h>
#define THREAD_STACK_SIZE                  1024*64

struct queue c_queue, i_queue;
pthread_t CEXEC;
pthread_t IEXEC;
static ucontext_t c_sche, i_sche; 
int size_c, size_i; // size of the c_queue, and i_queue.
thread_t *cur_t; // indicate the current running thread for c_scheduler.
request_t *cur_r; // indicate the current running thread for i_scheduler.
int isShutdown; // indicate if sut_shutdown() is called, 0 for not, 1 for called.
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; // Lock


void c_scheduler(){
    struct queue_entry *entry;
    while(true){
        // if there's no task left to run in both queue, and sut_shutdown() is called, exit.
        while (!size_c){
            if (!size_i && !cur_r && isShutdown)
                return;
            usleep(10);
        }
        // use mutex here to ensure only one is modifying the queue at any given time.
        pthread_mutex_lock(&m);
        entry = queue_pop_head(&c_queue);
        size_c --;
        cur_t = entry -> data;
        pthread_mutex_unlock(&m);

        // run the scheduled task.
        swapcontext(&c_sche, &(cur_t -> context));
    }
}

void i_scheduler() {
    struct queue_entry *entry;
    ssize_t ret;
    char msg[1024];
    while(true){
        // Similar to c_sche, used to check if the program need to stop
        while (!size_i)
        {
            if (!size_c && !cur_t && isShutdown)
                return;
            usleep(10);
        }
        pthread_mutex_lock(&m);
        entry = queue_pop_head(&i_queue);
        size_i--;
        cur_r = entry->data;
        pthread_mutex_unlock(&m);
        // opt is a enum indicate the type of the option, declared in sut.h
        switch (cur_r -> opt){
		case OPEN:
			ret = connect_to_server(cur_r->dest, cur_r->port, &(cur_r->thread->socketfd));
            if (ret < 0){
                printf("ERROR! Fail to connect to server\n");
            }
            else
			    cur_r->thread->isConnected = true;
			pthread_mutex_lock(&m);
			queue_insert_tail(&c_queue, queue_new_node(cur_r->thread));
			size_c ++;
			pthread_mutex_unlock(&m);
			break;
		case READ:
        // make msg ready for getting the info.
			memset(msg, 0, 1024);
			if (cur_r->thread->isConnected == false)
			{
				printf("FATAL: This task is not connected to any server!\n");
			}
			else
			{
				ret = recv_message(cur_r->thread->socketfd, msg, 1024);
                if (ret <0)
                {
                    printf("Error! Fail to receive messages");
                }
                else if (ret == 0)
                {
                    printf("Error! Current socket connection has been closed");
                }
                else
                    cur_r->thread->buf = msg;
			}
			pthread_mutex_lock(&m);
			queue_insert_tail(&c_queue, queue_new_node(cur_r->thread));
			size_c ++;
			pthread_mutex_unlock(&m);
            break;
        case WRITE:
			if (cur_r->thread->isConnected == false)
			{
				printf("Error !!! This task has not initialized with sut_init() call \n");
			}
			else
			{
            ret = send_message(cur_r -> thread ->socketfd, cur_r->buf, cur_r->size);
            if (ret <0){
                printf("Error in send messages");
            }
            }
			break;
		default:
			break;
		}
    }
}

void sut_init() {
    c_queue = queue_create();
    queue_init(&c_queue);
    i_queue = queue_create();
    queue_init(&i_queue);
    // use (void *) to avoid warning message
    pthread_create(&CEXEC, NULL, (void *)c_scheduler,0);
    pthread_create(&IEXEC, NULL, (void *)i_scheduler,0);
}

bool sut_create(sut_task_f fn){
    ucontext_t current;
    if(0!=(errno = getcontext(&current))){
        perror("sut_create() failed");
    }
    current.uc_stack.ss_sp = (char *) malloc (THREAD_STACK_SIZE);
    current.uc_stack.ss_size = THREAD_STACK_SIZE;
    current.uc_stack.ss_flags = 0;
    current.uc_link = 0;
    makecontext(&current, fn, 0);
    thread_t *t = (thread_t*) malloc (sizeof(thread_t));
    t->context = current;
    t-> socketfd= 0;
    struct queue_entry *node = queue_new_node(t);
    pthread_mutex_lock(&m);
    queue_insert_tail(&c_queue, node);
    size_c ++;
    pthread_mutex_unlock(&m);
    return true; 
}   

void sut_yield(){
    struct queue_entry *node = queue_new_node(cur_t);
    pthread_mutex_lock(&m);
    queue_insert_tail(&c_queue, node);
    size_c ++;
    pthread_mutex_unlock(&m);
    swapcontext(&(cur_t->context), &c_sche);
}

void sut_exit(){
    setcontext(&c_sche);
}

void sut_open(char *dest, int port){
    request_t *request = (request_t *)malloc(sizeof(request_t));
    request -> thread = cur_t;
    request-> dest = dest;
    request-> port = port;
    request->opt = OPEN;

    struct queue_entry *node = queue_new_node(request);
    pthread_mutex_lock(&m);
    queue_insert_tail(&i_queue,node);
    size_i ++;
    pthread_mutex_unlock(&m);
    swapcontext(&(cur_t->context), &c_sche);
}

void sut_write(char *buf, int size){
    request_t *request = (request_t *)malloc(sizeof(request_t));
    request ->thread = cur_t;
    request->size = size;
    request->buf = buf;
    request->opt = WRITE;
    struct queue_entry *node = queue_new_node(request);
    pthread_mutex_lock(&m);
    queue_insert_tail(&i_queue, node);
    size_i ++;
    pthread_mutex_unlock(&m);
}

void sut_close(){
    if(0 != (errno = shutdown(cur_t->socketfd,2)))
        perror("socket shutdown failed");
    cur_t -> isConnected = false;
}

char *sut_read(){
    request_t *request = (request_t *)malloc(sizeof(request));
    request ->thread = cur_t;
    request -> opt = READ;
    struct queue_entry *node = queue_new_node(request);
    pthread_mutex_lock(&m);
    queue_insert_tail(&i_queue, node);
    size_i ++;
    pthread_mutex_unlock(&m);
    swapcontext(&(cur_t->context), &c_sche);
    return cur_t -> buf;
}

void sut_shutdown(){
    isShutdown =1;
    if (0 != (errno = pthread_join(CEXEC,NULL)) && 0 != (errno = pthread_join(IEXEC,NULL)))
    {
        perror("pthread_join() failed");
    }
}