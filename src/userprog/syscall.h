#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <debug.h>

#define KILLED -1                 /* Exit code when user process is killed. */

void syscall_init (void);
void exit_util (int) NO_RETURN;
void filesys_acquire (void);
void filesys_release (void);

#endif /* userprog/syscall.h */
