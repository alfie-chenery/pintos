#include "syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include <console.h>
#include "process.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "pagedir.h"
#include "filesys/filesys.h"

struct lock filesys_lock;         /* Lock for the filesystem */

/* Acquire the lock for the filesystem */
static void
filesys_acquire (void)
{
  lock_acquire (&filesys_lock);
}

/* Releases the lock for the filesystem */
static void
filesys_release (void)
{
  lock_release (&filesys_lock);
}

static void exit_util (int);

/* Validates a pointer to a buffer passed by a user */
static void
validate_user_buffer (const void *buffer, unsigned size) 
{
  for (unsigned i = 0; i < size; i++, buffer++)
    {
      if (!is_user_vaddr (buffer)
          || pagedir_get_page (thread_current ()->pagedir, buffer) == NULL)
        exit_util (1);
    }
}

/* Validates a string passed by a user */
static void
validate_user_string (const char *string)
{
  for (;;)
    {
      if (!is_user_vaddr (string)
          || pagedir_get_page (thread_current ()->pagedir, string) == NULL)
        exit_util (1);

      if (*string == '\0')
        return;

      string++;
    }
}

/* Get the n th argument from an interrupt frame */
#define GET_ARG(f, n) ((int32_t *) f->esp + n)

static void 
halt (struct intr_frame *f)
{
  shutdown_power_off ();
}

/* Terminates a user process and prints exit code to standard output. */
static void
exit_util (int status)
{
  bool is_user_process = false;

  for (struct list_elem *elem = list_begin (&user_processes);
       elem != list_end (&user_processes);
       elem = list_next (elem))
    {
      struct pid_elem *p = list_entry (elem, struct pid_elem, elem);
      if (p->t == thread_current ())
        {
          is_user_process = true;
          p->exit_code = status;
        }
    }

  ASSERT (is_user_process);
  printf ("%s: exit(%d)", thread_name (), status);
  thread_exit ();
}

static void 
exit (struct intr_frame *f)
{
  int status = *GET_ARG (f, 1);
  exit_util (status);
}

static void 
exec (struct intr_frame *f)
{

}

static void 
wait (struct intr_frame *f)
{

}

static void
create (struct intr_frame *f)
{
  const char *file = (char *) GET_ARG (f, 1);
  unsigned initial_size = *GET_ARG (f, 2);
  validate_user_string (file);

  filesys_acquire ();
  f->eax = filesys_create (file, initial_size);
  filesys_release ();
}

static void 
remove (struct intr_frame *f)
{

}

static void 
open (struct intr_frame *f)
{
   
}

static void 
filesize (struct intr_frame *f)
{

}

static void 
read (struct intr_frame *f)
{

}

static void 
write (struct intr_frame *f)
{
  int fd = *GET_ARG (f, 1);
  const void *buffer = (void *) GET_ARG (f, 2);
  unsigned size = *GET_ARG (f, 3);

  validate_user_buffer (buffer, size);
}

static void 
seek (struct intr_frame *f)
{

}

static void
tell (struct intr_frame *f)
{

}

static void
close (struct intr_frame *f)
{

}

// sys_func represents a system call function called by syscall_handler
typedef void sys_func (struct intr_frame *);

// Array mapping sys_func to the corresponsing system call numbers
sys_func *sys_funcs[13] = {
  halt,
  exit,
  exec,
  wait,
  create,
  remove,
  open,
  filesize,
  read,
  write,
  seek,
  tell,
  close
};

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  printf ("system call!\n");
  sys_funcs[*GET_ARG (f, 0)] (f);
}

/*
static void
validate_user_memory (const void *user_memory)
{
  if (!is_user_vaddr (user_memory)
      || pagedir_get_page (thread_current ()->pagedir, user_memory) == NULL)
    exit (1);
}

void 
halt (void) 
{
  shutdown_power_off ();
}


void 
exit (int status) 
{
  char *proc_name = thread_current ()->name;
  struct thread *cur_thread = thread_current();
  bool is_userprog = false;
  struct list_elem *proc;

  for (proc = list_begin (&userprog_ids); proc != list_end (&userprog_ids);
       proc = list_next (proc)) 
    {
         struct process_id *thread_elem = 
          list_entry(proc, struct process_id, elem);
         if (thread_elem->pid == cur_thread->tid 
             && cur_thread == thread_elem->thread) 
          {
            is_userprog = true;
            break;
          }
    }

  if (is_userprog)
    {
      printf ("%s:  exit(%d)", proc_name, status);;
    }
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
  // Also check end
  validate_user_memory (buffer);

  if (fd == 1)
    {
      // Write to the console
      putbuf (buffer, length);
      return length;
    }

  return -1;
}

 bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned length);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd); */
