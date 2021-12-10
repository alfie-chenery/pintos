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
#include "vm/page.h"

static struct lock filesys_lock;      /* Lock for the filesystem. */
static int filesys_lock_depth;        /* How many times it has been acquired. */

/* Acquire the lock for the filesystem. */
void
filesys_acquire (void)
{
  if (lock_held_by_current_thread (&filesys_lock))
    filesys_lock_depth++;
  else
    lock_acquire (&filesys_lock);
}

/* Releases the lock for the filesystem. */
void
filesys_release (void)
{
  if (filesys_lock_depth > 0)
    filesys_lock_depth--;
  else
    lock_release (&filesys_lock);
}

/* Validates user pointer */
static void
validate_user_pointer (const void *p)
{
  if (!is_user_vaddr (p) || 
        (pagedir_get_page (thread_current ()->pagedir, p) == NULL &&
         !contains_vaddr (&thread_current ()->supplemental_page_table, 
                          pg_round_down (p))))
    exit_util (KILLED);
}

/* Validates a pointer to a buffer passed by a user */
static void
validate_user_buffer (const void *buffer, unsigned size) 
{
  /* Empty buffer is valid by default. */
  if (size == 0)
    return;

  /* Checking the end of the buffer is a valid user virtual address. If the end
     is less than PHYS_BASE, then certainly every other pointer in the buffer 
     would be valid as well. */
  if (!is_user_vaddr (buffer + size - 1))
    exit_util (KILLED);

  /* Check that the page directory in the beginning is fine. */
  validate_user_pointer (buffer);

  /* Go to the beginning of the next page. */
  const void *next_page = buffer + (PGSIZE - (int) buffer % PGSIZE);

  /* Check all the covered pages are valid. */
  for (; next_page < buffer + size; next_page += PGSIZE)
    validate_user_pointer (next_page);
}

/* Validates a string passed by a user. */
static void
validate_user_string (const char *string)
{
  /* Check that the beginning of the string is fine. */
  validate_user_pointer (string);

  /* Check if string is empty. */
  if (*string == '\0')
    return;

  /* Check that the string terminates at a valid user virtual address and all 
     the covered pages are valid. */
  for (const char *c = string + 1; is_user_vaddr (c); c++)
    {
      /* Check that c is a valid user virtual address. */
      if (!is_user_vaddr (c))
        exit_util (KILLED);

      /* If a page begins here then check that the page is valid. */
      if ((int) c % PGSIZE == 0)
        validate_user_pointer (c);

      /* If we have reached the end of the string then terminate. */
      if (*c == 0)
        return;
    }

  /* We are no longer in valid user virtual address space. */
  exit_util (KILLED);
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
  validate_user_pointer ((int32_t *) f->esp + n);
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

/* Returns the size in bytes of the file open as the given file descriptor, or 
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

/* Maps a file into virtual memory. The passed file descriptor must be valid, 
   the file must not have length 0. The passed address *addr must be page 
   aligned and the range of pages mapped must not overlap with any existing
   pages. Moreover, addr cannot be NULL and cannot be in the reserved stack 
   space. If no pre condition is violated, then the function returns a 
   "mapping ID" that uniquely identifies the mapping within the process. On 
   failure, it returns -1, which is not otherwise a valid mapping ID. */
static void
mmap_h (struct intr_frame *f)
{
  int fd = *get_arg (f, 1);
  void *addr = *(void **) get_arg (f, 2);
  struct file *file = file_from_fd (fd);
  f->eax = -1; /* Setting the default return value. */

  filesys_acquire ();
  int size = file == NULL ? 0 : file_length (file);
  filesys_release ();

  /* Checking that all the covered addresses are valid user addresses and not 
     saved for the stack. Note that only checking the last address is sufficient
     since the stack grows from the top to bottom. */
  void *last_addr = addr + size - 1;
  if (!is_user_vaddr (last_addr) || reserved_for_stack (last_addr))
    return;

  /* Checking if any pre-condition for mmap has been violated. */
  if (addr == NULL || size == 0 || addr != pg_round_down (addr))
    return;

  /* Checking that the range of pages that will be covered does not overlap with
     any existing page. */
  struct thread *t = thread_current ();
  for (void *page = addr; page < addr + size; page += PGSIZE)
    {
      if (contains_vaddr (&t->supplemental_page_table, page))
        return;
    }

  /* Malloc a mapid_elem for the mapping and check that it is not null. */
  struct mapid_elem *mapid = malloc (sizeof (struct mapid_elem));
  if (mapid == NULL)
    return;

  /* Reopening the file. */
  filesys_acquire ();
  file = file_reopen (file);
  filesys_release ();
  if (file == NULL)
    return;

  /* Add the required pages to the supplemental page table. */
  int ofs = 0;
  for (void *page = addr; page < addr + size; page += PGSIZE)
    {
      size_t bytes_read = addr + size - page > PGSIZE 
                          ? PGSIZE : addr + size - page;
      size_t zero_bytes = PGSIZE - bytes_read;

      /* Create a page_elem for the current page. */
      struct page_elem *page_elem 
          = create_page_elem (page, file, ofs, bytes_read, zero_bytes, true);

      /* Return if the page_elem could not be malloced. */
      if (page_elem == NULL)
        return;

      /* Mark the page_elem to be for an mmap. */
      page_elem->mmap = true;

      /* Add the page_elem to the supplemental page table. */
      insert_supplemental_page_entry (&t->supplemental_page_table, page_elem);
    }

  /* Add the mapid_elem to the current thread's list of mappings. */
  f->eax = mapid->mapid = t->next_mapid++;
  mapid->addr = addr;
  mapid->file = file;
  mapid->size = size;
  list_push_back (&t->mapids, &mapid->elem);
}

/* Writes back all the accessed pages of a mapped file to memory. */
void 
munmap_util (struct mapid_elem *mapid)
{
  struct thread *t = thread_current ();

  filesys_acquire ();

  /* Iterate through all the covered pages. */
  for (void *page = mapid->addr; page < mapid->addr + mapid->size; 
       page += PGSIZE)
    {
      struct page_elem *page_elem = 
          get_page_elem (&t->supplemental_page_table, page);
      ASSERT (page_elem != NULL);

      /* Write back to file if dirty bit is set. */
      void *kpage = pagedir_get_page (t->pagedir, page);
      if (pagedir_is_dirty (t->pagedir, page))
        {
          file_seek (mapid->file, page_elem->offset);
          file_write (mapid->file, kpage, page_elem->bytes_read);
        }

      /* Clear page directory and remove page from SPT. */
      pagedir_clear_page (t->pagedir, page);
      remove_page_elem (&t->supplemental_page_table, page_elem);
    }

  file_close (mapid->file);
  filesys_release ();
}

/* Unmaps a file from virtual memory. */
static void
munmap_h (struct intr_frame *f)
{
  mapid_t mapping = *get_arg (f, 1);

  /* Finding the mapid_elem for the given mapid. */
  struct thread *t = thread_current ();
  struct mapid_elem *mapid = NULL;
  for (struct list_elem *e = list_begin (&t->mapids);
       e != list_end (&t->mapids);
       e = list_next (e))
    {
      struct mapid_elem *cur = list_entry (e, struct mapid_elem, elem);
      if (cur->mapid == mapping)
        {
          mapid = cur;
          break;
        }
    }

  /* Check if invalid argument has been passed. */
  if (mapid == NULL)
    return;

  /* Writing the pages which have been changed back to the file. */
  munmap_util (mapid);

  /* Remove mapid from the list of mappings. */
  list_remove (&mapid->elem);
  free (mapid);
}

/* sys_func represents a system call function called by syscall_handler. */
typedef void sys_func (struct intr_frame *);

#define NUM_SYSCALLS 15

/* Array mapping sys_func to the corresponsing system call numbers. */
static sys_func *sys_funcs[NUM_SYSCALLS] = {
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
  close_h,
  mmap_h,
  munmap_h
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
  int syn_no = *get_arg (f, 0);
  if (syn_no < 0 || syn_no >= NUM_SYSCALLS)
    exit_util (KILLED);
  sys_funcs[*get_arg (f, 0)] (f);
}