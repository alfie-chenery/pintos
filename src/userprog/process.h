#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <list.h>
#include "user/syscall.h"

struct pid_elem
{
    pid_t pid;
    struct thread *t;
    int exit_code;
    struct list_elem elem;
};

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

struct list user_processes;

#endif /* userprog/process.h */
