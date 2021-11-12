#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
#include <list.h>
#include "user/syscall.h"

/* Struct to store user processes and their exit codes. */
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
struct lock user_processes_lock;

#endif /* userprog/process.h */
