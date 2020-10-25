#include <stdbool.h>
#include "sut.h"
#include "queue.h"
#include <pthread.h>
#include <errno.h>

#define THREAD_STACK_SIZE                  1024*64
struct queue q;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_t CEXEC;
pthread_t IEXEC;

void sut_init(){
    q = queue_create();
    pthread_create(&CEXEC, NULL, NULL,NULL);
    pthread_create(&IEXEC, NULL, NULL, NULL);

}

bool sut_create(sut_task_f fn){
    thread *threadptr;
    getcontext(&(threadptr ->threadcontext));
    threadptr -> threadstack = (char *) malloc (THREAD_STACK_SIZE);
    threadptr -> threadcontext.uc_stack.ss_sp = threadptr -> threadstack;
    threadptr -> threadcontext.uc_stack.ss_size = THREAD_STACK_SIZE;
    threadptr -> threadcontext.uc_link = 0;
    threadptr -> threadcontext.uc_stack.ss_flags = 0;
    threadptr -> threadfunc = fn;
    makecontext(&(threadptr -> threadcontext), fn, 0);
    struct queue_entry *node = queue_new_node(threadptr);
    queue_insert_tail(&q, node);
    return true;
}

void sut_yield(){

}

void sut_exit(){

}

void sut_open(char *dest, int port){

}

void sut_write(char *buf, int size){

}

void sut_close(){

}

char *sut_read(){

}

void sut_shutdown(){
    if (0 != (errno = pthread_join(CEXEC,NULL)) && 0 != (errno = pthread_join(IEXEC,NULL)))
    {
        perror("pthread_join() failed");
    }
}