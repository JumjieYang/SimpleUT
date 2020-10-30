#ifndef __SUT_H__
#define __SUT_H__
#include <stdbool.h>
#include <ucontext.h>

typedef enum option{
    OPEN, READ, WRITE
} option;
typedef struct thread_t {
    ucontext_t context;
    int socketfd;
} thread_t;

typedef struct request_t {
    thread_t thread;
    option opt;
    int port,size;
    char *dest, *buf;
} request_t;

typedef void (*sut_task_f)();

void sut_init();
bool sut_create(sut_task_f fn);
void sut_yield();
void sut_exit();
void sut_open(char *dest, int port);
void sut_write(char *buf, int size);
void sut_close();
char *sut_read();
void sut_shutdown();


#endif
