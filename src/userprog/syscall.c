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

static struct lock filesys_lock;         /* Lock for the filesystem. */

/* Acquire the lock for the filesystem. */
void
filesys_acquire (void)
{
  lock_acquire (&filesys_lock);
}

/* Releases the lock for the filesystem. */
void
filesys_release (void)
{
  lock_release (&filesys_lock);
}

/* Validates a pointer to a buffer passed by a user */
static void
validate_user_buffer (const void *buffer, unsigned size) 
{
  /* Going throughout the buffer rather than just checking the start. */
  for (unsigned i = 0; i < size; i++, buffer++)
    {
      if (!is_user_vaddr (buffer)
          || pagedir_get_page (thread_current ()->pagedir, buffer) == NULL)
        /* Killing the user process since some part of buffer is invalid. */
        exit_util (KILLED);
    }
}

/* Validates a string passed by a user. */
static void
validate_user_string (const char *string)
{
  for (;;)
    {
      if (!is_user_vaddr (string)
          || pagedir_get_page (thread_current ()->pagedir, string) == NULL)
        /* Killing the user process since some part of string is invalid. */
        exit_util (KILLED);

      if (*string == '\0')
        return;

      string++;
    }
}

/* Returns a file pointer from a file descriptor, or null if file descriptor is 
   invalid. */
static struct file *
file_from_fd (int fd)
{
  /* Iterate through thread's fds and find the correct one. */
  struct thread *t = thread_current ();
  for (struct list_elem *elem = list_begin (&t->fds);
       elem != list_end (&t->fds);
       elem = list_next (elem))
    {
      struct fd_elem *fd_elem = list_entry (elem, struct fd_elem, elem);
      if (fd_elem->fd == fd)
        return fd_elem->file;
    }
  
  /* Incorrect fd has been passed. */
  return NULL;
}

/* Get the n th argument from an interrupt frame. */
static int32_t *
get_arg (const struct intr_frame *f, int n)
{
  /* Checking that the given argument is pointing to valid memory. */
  validate_user_buffer ((int32_t *) f->esp + n, sizeof (int32_t));
  return (int32_t *) f->esp + n;
}

/* Terminates Pintos. */
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

  /* Unblocking parent thread if it had called wait/process_wait. */
  sema_up (&thread_current ()->user_elem->s);
  thread_exit ();
}

/* Retrieves exit code from interrupt frame and calls exit_util. */
static void 
exit_h (struct intr_frame *f)
{
  int status = *get_arg (f, 1);
  exit_util (status);
}

/* Runs the executable whose name and arguments are passed on the interrupt 
   frame. Returns -1 if the new process could not load or run, otherwise returns
   the new process's program id. Hence, waits until the child process has 
   finished load. */
static void 
exec_h (struct intr_frame *f)
{
  const char *exec_name = *(char **) get_arg (f, 1);
  validate_user_string (exec_name);
  f->eax = process_execute (exec_name);
}

/* Waits for a child process with a given pid and returns its exit code. */
static void 
wait_h (struct intr_frame *f)
{
  pid_t pid = (pid_t) *get_arg (f, 1);
  f->eax = process_wait (pid);
}

/* Creates a file with a given name and size and returns true if the operation 
   was successful and false otherwise. */
static void
create_h (struct intr_frame *f)
{
  const char *name = *(char **) get_arg (f, 1);
  unsigned initial_size = *get_arg (f, 2);
  validate_user_string (name);

  filesys_acquire ();
  f->eax = filesys_create (name, initial_size);
  filesys_release ();
}

/* Deletes the file of the given name. Returns true if successful, false 
   otherwise. */
static void 
remove_h (struct intr_frame *f)
{
  const char *name = *(char **) get_arg (f, 1);
  validate_user_string (name);

  filesys_acquire ();
  f->eax = filesys_remove (name);
  filesys_release();
}

/* Opens the file whose name is passed on the interrupt frame. Returns a 
   nonnegative integer handle called a “file descriptor” (fd), or -1 if the file
   could not be opened. */
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
      /* Could not open file. */
      f->eax = -1;
      return;
    }

  /* Add current file to the thread's list of fds. */
  struct fd_elem *fd = malloc (sizeof (struct fd_elem));
  if (fd == NULL)
    {
      /* No more memory is left to open more files. */
      file_close (file);
      f->eax = -1;
      return;
    }

  fd->fd = thread_current ()->next_fd;
  fd->file = file;
  list_push_back (&thread_current ()->fds, &fd->elem);
  f->eax = thread_current ()->next_fd++;
}

/* Returns the size in bytes of the file open as the given file descriptor, or \
   -1 if the file descriptor is invalid. */
static void 
filesize_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  struct file *file = file_from_fd (fd);

  if (file == NULL)
    {
      /* Invalid file descriptor. */
      f->eax = -1;
      return;
    }

  filesys_acquire ();
  f->eax = file_length (file);
  filesys_release ();
}

/* Reads size bytes from the file open as the given file descriptor into buffer.   
   Returns the number of bytes actually read or -1 if the file could not be 
   read. Reads from the keyboard if fd is 0. */
static void 
read_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  void *buffer = *(void **) get_arg (f, 2);
  unsigned size = *get_arg (f, 3);
  validate_user_buffer (buffer, size);
  f->eax = -1; /* Setting the default return value. */

  if (fd == 0) 
    {
      /* Storing the keyboard character input in the buffer for size bytes */
      for (unsigned i = 0; i < size; i++) 
        *((char*) buffer) = input_getc ();
      f->eax = size;
    }
  else
    {
      /* Reading from a file. */
      struct file *file = file_from_fd (fd);
      if (file == NULL)
        return;

      filesys_acquire ();
      f->eax = file_read (file, buffer, size);
      filesys_release (); 
    }
}

/* Writes size bytes from the file open as fd to buffer. Returns the number of 
   bytes actually written. Fd 0 writes to the console. Returns -1 if an 
   incorrect fd is passed. */
static void 
write_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  const void *buffer = *(void **) get_arg (f, 2);
  unsigned size = *get_arg (f, 3);
  f->eax = -1; /* Setting the default return value. */

  validate_user_buffer (buffer, size);

  if (fd == 1)
    {
      /* Writing to console. */
      putbuf (buffer, size);
      f->eax = size;
    }
  else
    {
      /* Writing to a file. */
      struct file *file = file_from_fd (fd);
      if (file == NULL)
        return;
      
      filesys_acquire ();
      f->eax = file_write (file, buffer, size);
      filesys_release ();
    }
}

/* Change the next byte to read or write in an open file. */
static void 
seek_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  unsigned position = *get_arg (f, 2);
  struct file *file = file_from_fd (fd);
  if (file == NULL)
    return;

  filesys_acquire ();
  file_seek (file, position);
  filesys_release ();
}

/* Returns the next byte to read or write in an open file fd. Returns -1 if the 
   file descriptor is invalid. */
static void
tell_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  struct file *file = file_from_fd (fd);
  f->eax = -1;
  if (file == NULL)
    return;

  filesys_acquire ();
  f->eax = file_tell (file);
  filesys_release ();
}

/* Closes file descriptor fd. */
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
          free (fd_elem); /* We free it since it was malloced in open_h. */
          break;
        }
    }
}

/* sys_func represents a system call function called by syscall_handler. */
typedef void sys_func (struct intr_frame *);

/* Array mapping sys_func to the corresponsing system call numbers. */
static sys_func *sys_funcs[13] = {
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