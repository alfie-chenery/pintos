#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include <lib/user/syscall.h>
#include "threads/synch.h"
#include "process.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  thread_exit ();
}

void 
halt (void) 
{
  shutdown_power_off ();
}


void 
exit (int status) 
{
  char* proc_name = thread_current ()->name;
  struct thread* cur_thread = thread_current();
  bool is_userprog = false;
  struct list_elem* proc;

  for (proc = list_begin (&userprog_ids); proc != list_end (&userprog_ids);
       proc = list_next (proc)) 
       {
         struct process_id* thread_elem = list_entry(proc, struct process_id, elem);
         if (thread_elem->pid == cur_thread->tid && cur_thread == thread_elem->thread) 
          {
            is_userprog = true;
            break;
          }
       }

  if (!is_userprog)
    {
      return;
    }

  printf ("%s:  exit(%d)", proc_name, status);
}

pid_t 
exec (const char *file) 
{
  return -1;
}

int 
wait (pid_t pid) 
{
  return -1;
}

int 
write (int fd, const void *buffer, unsigned length)
{

}

/* bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd); */