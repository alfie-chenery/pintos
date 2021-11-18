#include "syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "process.h"
#include "lib/user/syscall.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

static struct lock filesys_lock;         /* Lock for the filesystem */

/* Acquire the lock for the filesystem */
void
filesys_acquire (void)
{
  lock_acquire (&filesys_lock);
}

/* Releases the lock for the filesystem */
void
filesys_release (void)
{
  lock_release (&filesys_lock);
}

/* Validates a pointer to a buffer passed by a user */
static void
validate_user_buffer (const void *buffer, unsigned size) 
{
  for (unsigned i = 0; i < size; i++, buffer++)
    {
      if (!is_user_vaddr (buffer)
          || pagedir_get_page (thread_current ()->pagedir, buffer) == NULL)
        exit_util (KILLED);
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
        exit_util (KILLED);

      if (*string == '\0')
        return;

      string++;
    }
}

/* Returns a file pointer from a file descriptor. */
static struct file *
file_from_fd (int fd)
{
  // Iterate through thread's fds and find the correct one.
  struct thread *t = thread_current ();
  for (struct list_elem *elem = list_begin (&t->fds);
       elem != list_end (&t->fds);
       elem = list_next (elem))
    {
      struct fd_elem *fd_elem = list_entry (elem, struct fd_elem, elem);
      if (fd_elem->fd == fd)
        return fd_elem->file;
    }
  
  // Incorrect fd has been passed.
  exit_util (KILLED);
  NOT_REACHED ();
}

/* Get the n th argument from an interrupt frame */
static int32_t *
get_arg (const struct intr_frame *f, int n)
{
  validate_user_buffer ((int32_t *) f->esp + n, sizeof (int32_t));
  return (int32_t *) f->esp + n;
}

static void 
halt_h (struct intr_frame *f UNUSED)
{
  shutdown_power_off ();
}

/* Terminates a user process and prints exit code to standard output. */
void
exit_util (int status)
{
  /* Update user_elem of current thread */
  thread_current ()->user_elem->exit_code = status;
  printf ("%s: exit(%d)\n", thread_name (), status);
  sema_up (&thread_current ()->user_elem->s);
  thread_exit ();
}

static void 
exit_h (struct intr_frame *f)
{
  int status = *get_arg (f, 1);
  exit_util (status);
}

static void 
exec_h (struct intr_frame *f)
{
  const char *exec_name = *(char **) get_arg (f, 1);
  validate_user_string (exec_name);
  f->eax = process_execute (exec_name);
}

static void 
wait_h (struct intr_frame *f)
{
  pid_t pid = (pid_t) *get_arg (f, 1);
  f->eax = process_wait (pid);
}

static void
create_h (struct intr_frame *f)
{
  const char *file = *(char **) get_arg (f, 1);
  unsigned initial_size = *get_arg (f, 2);
  validate_user_string (file);

  filesys_acquire ();
  f->eax = filesys_create (file, initial_size);
  filesys_release ();
}

static void 
remove_h (struct intr_frame *f)
{
  const char *name = *(char **) get_arg (f, 1);
  validate_user_string (name);

  /* not sure if this is unix like semantics */
  filesys_acquire ();
  f->eax = filesys_remove (name);
  filesys_release();
}

static void 
open_h (struct intr_frame *f)
{
  const char *name = *(char **) get_arg (f, 1);
  validate_user_string (name);

  filesys_acquire ();
  struct file *file = filesys_open (name);
  filesys_release ();

  if (file == NULL)
    {
      // Could not open file.
      f->eax = -1;
      return;
    }

  // Add current file to the thread's list of fds
  struct fd_elem *fd = malloc (sizeof (struct fd_elem));
  fd->fd = thread_current ()->next_fd;
  fd->file = file;
  list_push_back (&thread_current ()->fds, &fd->elem);
  f->eax = thread_current ()->next_fd++;
}

static void 
filesize_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  struct file *file = file_from_fd (fd);

  filesys_acquire ();
  f->eax = file_length (file);
  filesys_release ();
}

static void 
read_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  void *buffer = *(void **) get_arg (f, 2);
  unsigned size = *get_arg (f, 3);
  validate_user_buffer (buffer, size);
  struct file *file = file_from_fd (fd);

  if (fd == 0) 
    {
      if (size > 0)
        {
          /* storing the character input in the buffer for size bytes */
          for (unsigned i = 0; i < size; i++) 
            {
              *((char*) buffer) = input_getc ();
            }
          f->eax = size;
          return;
        }
    }
  else 
    {
      filesys_acquire ();
      f->eax = file_read (file, buffer, size);
      filesys_release (); 
    }
}

static void 
write_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  const void *buffer = *(void **) get_arg (f, 2);
  unsigned size = *get_arg (f, 3);

  validate_user_buffer (buffer, size);

  if (fd == 1)
    {
      putbuf (buffer, size);
      f->eax = size;
    }
  else
    {
      /* create file from fd once first check has been performed */
      struct file *file = file_from_fd (fd);
      filesys_acquire ();
      f->eax = file_write (file, buffer, size);
      filesys_release ();
    }
}

static void 
seek_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  unsigned position = *get_arg (f, 2);
  struct file *file = file_from_fd (fd);

  filesys_acquire ();
  file_seek (file, position);
  filesys_release ();
}

static void
tell_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  struct file *file = file_from_fd (fd);

  filesys_acquire ();
  f->eax = file_tell (file);
  filesys_release ();
}

static void
close_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  struct file *file = file_from_fd (fd);

  filesys_acquire ();
  file_close (file);
  filesys_release ();

  /* Removing fd from the thread's list of open fds. */
  for (struct list_elem *elem = list_begin (&thread_current ()->fds);
       elem != list_end (&thread_current ()->fds);
       elem = list_next (elem))
    {
      struct fd_elem *fd_elem = list_entry (elem, struct fd_elem, elem);
      if (fd_elem->fd == fd)
        {
          list_remove (elem);
          free (fd_elem);
          break;
        }
    }
}

// sys_func represents a system call function called by syscall_handler
typedef void sys_func (struct intr_frame *);

// Array mapping sys_func to the corresponsing system call numbers
sys_func *sys_funcs[13] = {
  halt_h,
  exit_h,
  exec_h,
  wait_h,
  create_h,
  remove_h,
  open_h,
  filesize_h,
  read_h,
  write_h,
  seek_h,
  tell_h,
  close_h
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
  sys_funcs[*get_arg (f, 0)] (f);
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
