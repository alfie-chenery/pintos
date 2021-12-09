#include "userprog/process.h"
#include <debug.h>
#include "threads/malloc.h"
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include <list.h>
#include "vm/frame.h"
#include "vm/page.h"

#define USER_STACK_PAGE_SIZE 4096
#define USER_STACK_BASE_SIZE 12
#define KB_TO_BYTES 1024
#define MAX_USER_PROCESS_STACK_SPACE (2 * KB_TO_BYTES * KB_TO_BYTES)

/* Creates a user_elem for a new process */
static struct user_elem *
create_user_elem (void)
{
  struct user_elem *u = malloc (sizeof (struct user_elem));
  if (u == NULL)
    return u;
  sema_init (&u->s, 0);
  lock_init (&u->lock);
  u->rem = 2;
  u->load_successful = false;
  return u;
}

/* Parent or child has exited for a user_elem. */
static void
parent_or_child_exited (struct user_elem *u)
{
  bool to_free = false;

  lock_acquire (&u->lock);
  u->rem--;
  to_free = u->rem == 0;
  lock_release (&u->lock);

  /* Free the block of memory if both parent and child have exited */
  if (to_free)
    free (u);
}

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Parses the arguments from the file name passed as a parameter into 
   the argument vector passed */
static int
parse_args (char **argv, void *fn_copy)
{
  // tokenise file name into command and parameters
  char *token;
  char *save_ptr;
  int argc = 0;
  for (token = strtok_r (fn_copy, " ", &save_ptr);
       token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      argv[argc] = token;
      argc++;
    }
  argv[argc] = NULL;
  return argc;
}

static int
get_arg_count (char *fname)
{
  char *token;
  char *save_ptr;
  int argc = 0;
  for (token = strtok_r (fname, " ", &save_ptr);
       token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      argc++;
    }
  return argc;
}

/* Starts a new thread running a user program loaded from
   FILENAME. The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t 
process_execute (const char *file_name)
{
  char *fn_copy;
  char *fname;
  tid_t tid;

  /* if arguements size is greater than USER_STACK_PAGE_SIZE return TID_ERROR */
  if (strlen (file_name) > USER_STACK_PAGE_SIZE)
    return TID_ERROR;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  fname = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);
  strlcpy (fname, file_name, PGSIZE);

  int arg_count = get_arg_count (fname);
  palloc_free_page (fname);
  /* argument vector to mantain arguments to command */
  char *argv[arg_count];
  ASSERT (fn_copy != NULL);
  ASSERT (file_name != NULL);
  ASSERT (argv != NULL);

  /* parsing the arguments and generating the argument vector */
  int argc = parse_args (argv, fn_copy);
  
  /* checking if the stack can be fit in the given user stack space */
  if (argc * sizeof (char *) + sizeof(fn_copy) + USER_STACK_BASE_SIZE > 
      USER_STACK_PAGE_SIZE)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  /* Create user_elem for the child process. */
  struct user_elem *u = create_user_elem ();

  /* Checking there was enough memory available to create the user_elem. */
  if (u == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  list_push_back (&thread_current ()->children, &u->elem);
  struct pair p;
  p.first = argv;
  p.second = u;

  /* Create a new thread to execute FILE_NAME, passing arguments from array and 
     user_elem u as a pair to aux */
  tid = thread_create (argv[0], PRI_DEFAULT, start_process, &p);
  if (tid == TID_ERROR)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  u->tid = tid;

  /* Wait on child till load is done. */
  sema_down (&u->s);
  palloc_free_page (fn_copy);

  return u->load_successful ? tid : TID_ERROR;
}

/* Sets up the user stack according the argument vector passed from 
   start_process and shifts the stack pointer of the interrupt frame passed 
   by reference */
static bool 
user_stack_set_up (char **argv, struct intr_frame *intrf)
{
  /* store number of arguments in argument counter */
  int argc = 0;
  while (argv[argc] != NULL)
    argc++;

  /* pushing the strings in reverse order onto the stack */
  for (int i = argc - 1; i > -1; i--)
    {
      char *curr_arg = argv[i];
      ASSERT (curr_arg != NULL);

      /* copying the contents of the string onto the stack */
      intrf->esp -= strlen (curr_arg) + 1;
      strlcpy ((char *) intrf->esp, curr_arg, strlen (curr_arg) + 1);
      /* setting the stack address of the string in the argument vector */
      argv[i] = intrf->esp;   
    }

  /* rounding down the stack pointer to multiple of 4 for alignment */
  intrf->esp -= (uintptr_t) intrf->esp % 4;
  /* pushing the 0 uint8_t value onto the stack */
  intrf->esp -= sizeof (int);
  *(uint8_t *) intrf->esp = 0;

  /* pushing the argument vector adresses onto the stack */
  for (int i = argc; i > -1; i--)
    {
      intrf->esp -= sizeof (char *);
      *(char **) intrf->esp = argv[i];
    }

  /* push argv onto the stack  */
  intrf->esp -= sizeof (char **);
  *(char ***) intrf->esp = intrf->esp + sizeof (char **);
  /* push argc onto the stack  */
  intrf->esp -= sizeof (int);
  *(int *) intrf->esp = argc;
  /* pushing the sentinel void pointer onto the stack */
  intrf->esp -= sizeof (void **);
  *(void **) intrf->esp = 0;
  return true;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *command_information)
{
  /* command_information stores a pair. */
  char **argv = ((struct pair *) command_information)->first;
  thread_current ()->user_elem = ((struct pair *) command_information)->second;
  thread_current ()->user_elem->tid = thread_current ()->tid;

  /* Initialize the supplemental page table. */
  supplemental_page_table_init (&thread_current ()->supplemental_page_table);
    
  ASSERT (argv != NULL);
  struct intr_frame intrf;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&intrf, 0, sizeof intrf);
  intrf.gs = intrf.fs = intrf.es = intrf.ds = intrf.ss = SEL_UDSEG;
  intrf.cs = SEL_UCSEG;
  intrf.eflags = FLAG_IF | FLAG_MBS;
  success = load (argv[0], &intrf.eip, &intrf.esp);

  /* If load failed, quit. */
  if (!success)
    {
      sema_up (&thread_current ()->user_elem->s);
      exit_util (KILLED);
    }

  thread_current ()->user_elem->load_successful = true;

  bool stack_setup_success = user_stack_set_up (argv, &intrf);

  /* Let parent know that load was successful. Note that we cannot move this 
     above since argv was declared on the stack of the parent thread when it was
     in process_execute. */
  sema_up (&thread_current ()->user_elem->s);

  /* if stack cannot be setup kill the process */
  if (!stack_setup_success)
    exit_util (KILLED);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit"
                :
                : "g"(&intrf)
                : "memory"
               );
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status. 
 * If it was terminated by the kernel (i.e. killed due to an exception), 
 * returns -1.  
 * If TID is invalid or if it was not a child of the calling process, or if 
 * process_wait() has already been successfully called for the given TID, 
 * returns -1 immediately, without waiting. */
int 
process_wait (tid_t child_tid)
{
  struct user_elem *user_proc = NULL;

  /* searching for child thread with same tid as child_tid */
  for (struct list_elem *elem = list_begin (&thread_current ()->children);
       elem != list_end (&thread_current ()->children);
       elem = list_next (elem))
    {
      struct user_elem *u = list_entry (elem, struct user_elem, elem);
      if (u->tid == child_tid)
        {
          user_proc = u;
          break;
        }
    }

  /* check if wait has already been called or tid is invalid */
  if (user_proc == NULL)
    return -1;

  /* waiting if child process has not already exited */
  sema_down (&user_proc->s);

  /* storing exit code */
  int exit_code = user_proc->exit_code;

  /* Since wait cannot be called on the same thread multiple times, we delete 
     this process from the list. user_proc no longer has any more use. Hence we
     set its parent to exited so that the allocated memory can be freed. */
  list_remove (&user_proc->elem);
  parent_or_child_exited (user_proc);

  return exit_code;
}

/* Free the current process's resources. */
void 
process_exit (void)
{
  struct thread *cur = thread_current ();

  /* Unblocking parent thread if it had called wait/process_wait. */
  sema_up (&cur->user_elem->s);

  /* Set parent exited for all user_elem where current thread is the parent. */
  for (struct list_elem *elem = list_begin (&cur->children);
       elem != list_end (&cur->children);)
    {
      struct user_elem *u = list_entry (elem, struct user_elem, elem);

      /* Note that we must store the next element in elem before calling 
         parent_or_child_exited since the function might free u, because of 
         which deferencing elem would trigger a page fault. */
      elem = list_next (elem);
      parent_or_child_exited (u);
    }
  
  /* set child exited for current thread's user elem */
  parent_or_child_exited (cur->user_elem);

  /* Closing any open files. */
  filesys_acquire ();
  while (!list_empty (&cur->fds))
    {
      struct list_elem *elem = list_begin (&cur->fds);
      struct fd_elem *fd_elem = list_entry (elem, struct fd_elem, elem);
      file_close (fd_elem->file);
      list_remove (elem);
      free (fd_elem);
    }
  filesys_release ();

  /* Unmapping any mapped files. */
  while (!list_empty (&cur->mapids))
    {
      struct list_elem *elem = list_begin (&cur->mapids);
      struct mapid_elem *mapid = list_entry (elem, struct mapid_elem, elem);
      munmap_util (mapid);
      list_remove (elem);
      free (mapid);
    }

  /* Destroying the supplemental page table. */
  supplemental_page_table_destroy (&cur->supplemental_page_table);

  /* Closing the rox opened on load. */
  filesys_acquire ();
  file_close (cur->loaded_file);
  filesys_release ();

  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void 
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool 
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* Open executable file. */
  filesys_acquire ();
  file = filesys_open (file_name);
  if (file == NULL)
    {
      printf("load: %s: open failed\n", file_name);
      goto done;
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr || 
      memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || 
      ehdr.e_machine != 3 || ehdr.e_version != 1 || 
      ehdr.e_phentsize != sizeof (struct Elf32_Phdr) || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                            Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                            Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *)mem_page,
                                read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void))ehdr.e_entry;

  success = true;
  file_deny_write (file);

  thread_current ()->loaded_file = file;

done:
  filesys_release ();
  /* We arrive here whether the load is successful or not. */  
  return success;
}

/* load() helpers. */

bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy (), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
             uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs(upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  // file_seek (file, ofs);
  off_t ofs_curr = ofs;
  while (read_bytes > 0 || zero_bytes > 0)
  {
    /* Calculate how to fill this page.
        We will read PAGE_READ_BYTES bytes from FILE
        and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    struct thread *t = thread_current();

    struct hash supplemental_page_table = t->supplemental_page_table;
    struct page_elem *page =
        create_page_elem(upage, file, ofs_curr, page_read_bytes,
                         page_zero_bytes, writable);

    if (page == NULL)
      return false;

    if (!writable)
      page->rox = true;
    insert_supplemental_page_entry(&supplemental_page_table, page);

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
    ofs_curr += page_read_bytes;
  }

  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp)
{
  allocate_stack_page (PHYS_BASE - PGSIZE);
  *esp = PHYS_BASE;
  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
} 

/* Checks if an address is reserved for the stack. */
bool
reserved_for_stack (void *vaddr)
{
  return vaddr + MAX_USER_PROCESS_STACK_SPACE >= PHYS_BASE &&
         is_user_vaddr (vaddr);
}