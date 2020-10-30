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
thread_t *current;
int sockfd;
int *isShutdown;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;


void *c_scheduler(){
    struct queue_entry *entry;
    getcontext(&c_sche);
    while(true){
        entry = queue_peek_front(&c_queue);
        if (entry == NULL && isShutdown && NULL ==queue_peek_front(&i_queue))
            return;
        else if (entry != NULL){
            current = (thread_t *)entry ->data;
            pthread_mutex_lock(&m);
            queue_pop_head(&c_queue);
            pthread_mutex_unlock(&m);
            swapcontext(&c_sche,&current->context);
        }
        else{
            usleep(10);
        }
    }
}

void *i_scheduler() {
    struct queue_entry *entry;
    request_t *temp = (request_t *)malloc(sizeof(request_t));
    thread_t *temp_thread;
    char msg[1024];
    ssize_t ret;
    getcontext(&i_sche);
    while(true){
        entry = queue_peek_front(&i_queue);
        if (entry == NULL && isShutdown && NULL ==queue_peek_front(&c_queue))
            return;
        else if (entry != NULL){
            temp = (request_t *)entry ->data;
            pthread_mutex_lock(&m);
            queue_pop_head(&c_queue);
            pthread_mutex_unlock(&m);
            switch (temp->opt)
            {
            case OPEN:
                temp_thread = &temp->thread;
                connect_to_server(temp->dest,temp->port,&temp_thread->socketfd);
                struct queue_entry *node = queue_new_node(temp_thread);
                pthread_mutex_lock(&m);
                queue_insert_tail(&c_queue,node);
                pthread_mutex_unlock(&m);
                break;
            case READ:
                memset(temp->buf, 0, 1024);
                ret = recv_message(temp_thread->socketfd,temp->buf,1024);
                if (ret< 0)
                    printf("receive message error\n");
                else if (ret == 0)
                    printf("This task isn't connected to any server");
                else{
                    pthread_mutex_lock(&m);
                    queue_insert_tail(&c_queue,queue_new_node(temp_thread));
                    pthread_mutex_unlock(&m);
                }
                break;
            case WRITE:
                ret = send_message(temp_thread ->socketfd, temp->buf, strlen(msg));
                if (ret < 0)
                    printf("Error!");
                break;
            default:
                break;
            }
        }
        else{
            usleep(10);
        }
    }
}
void sut_init(){
    isShutdown = 0;
    c_queue = queue_create();
    queue_init(&c_queue);
    i_queue = queue_create();
    queue_init(&i_queue);
    pthread_create(&CEXEC, NULL, c_scheduler,&m);
    pthread_create(&IEXEC, NULL, i_scheduler,&m);

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
    pthread_mutex_unlock(&m);
    return true; 
}   

void sut_yield(){
    struct queue_entry *node = queue_new_node(current);
    pthread_mutex_lock(&m);
    queue_insert_tail(&c_queue, node);
    pthread_mutex_unlock(&m);
    swapcontext(&(current->context), &c_sche);
}

void sut_exit(){
    setcontext(&c_sche);
}

void sut_open(char *dest, int port){
    request_t *request = (request_t *)malloc(sizeof(request_t));
    request -> thread = *current;
    request-> dest = dest;
    request-> port = port;
    request->opt = OPEN;

    struct queue_entry *node = queue_new_node(request);
    pthread_mutex_lock(&m);
    queue_insert_tail(&i_queue,node);
    pthread_mutex_unlock(&m);
    swapcontext(&(current->context), &c_sche);
}

void sut_write(char *buf, int size){
    request_t *request = (request_t *)malloc(sizeof(request_t));
    request ->thread = *current;
    request->size = size;
    request->buf = buf;
    request->opt = WRITE;
    struct queue_entry *node = queue_new_node(request);
    pthread_mutex_lock(&m);
    queue_insert_tail(&i_queue, node);
    pthread_mutex_unlock(&m);
}

void sut_close(){
    if(0 != (errno = shutdown(current->socketfd,2)))
        perror("socket shutdown failed");
}

char *sut_read(){
    request_t *request = (request_t *)malloc(sizeof(request));
    request ->thread = *current;
    request -> opt = READ;

    struct queue_entry *node = queue_new_node(request);
    pthread_mutex_lock(&m);
    queue_insert_tail(&i_queue, node);
    pthread_mutex_unlock(&m);
    swapcontext(&(current->context), &c_sche);
    return request->buf;
}

void sut_shutdown(){
    isShutdown = (int *)1;
    if (0 != (errno = pthread_join(CEXEC,NULL)) && 0 != (errno = pthread_join(IEXEC,NULL)))
    {
        perror("pthread_join() failed");
    }
}