#include "vm/page.h"
#include <debug.h>
#include "threads/malloc.h"
#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "vm/share.h"
#include <string.h>

/* Calculates the hash for a page_elem. */
static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  void *vaddr = hash_entry (e, struct page_elem, elem)->vaddr;
  return hash_int ((int) vaddr);
}

/* Compares two page_elem. */
static bool
page_less (const struct hash_elem *a, 
           const struct hash_elem *b, 
           void *aux UNUSED)
{
  void *address_a = hash_entry (a, struct page_elem, elem)->vaddr;
  void *address_b = hash_entry (b, struct page_elem, elem)->vaddr;
  return address_a < address_b;
}

/* Initializes a supplemental page table. */
void 
supplemental_page_table_init (struct hash *supplemental_page_table)
{
  hash_init (supplemental_page_table, page_hash, page_less, NULL);
}

/* Inserting a page element into the supplemental page table. */
void 
insert_supplemental_page_entry (struct hash *supplemental_page_table, 
                                struct page_elem *page)
{
  hash_replace (supplemental_page_table, &page->elem);
}

/* Smart constructor to create a page_elem */
struct page_elem * 
create_page_elem (void *vaddr, struct file *file, size_t offset,
                  size_t bytes_read, size_t zero_bytes, bool writable)
{
  struct page_elem *page = malloc (sizeof (struct page_elem));
  if (page == NULL)
    return page;

  page->vaddr = vaddr;
  page->file = file;
  page->offset = offset;
  page->bytes_read = bytes_read;
  page->zero_bytes = zero_bytes;
  page->writable = writable;
  page->rox = false;
  return page;
}

/* Check if supplemental page table contains vaddr */
bool
contains_vaddr (struct hash *supplemental_hash_table, void *vaddr)
{
  struct page_elem page;
  page.vaddr = vaddr;
  return hash_find (supplemental_hash_table, &page.elem) != NULL;
}

/* Get page_elem for a specific vaddr. */
struct page_elem *
get_page_elem (struct hash *supplemental_hash_table, void *vaddr)
{
  struct page_elem page;
  page.vaddr = vaddr;
  struct hash_elem *elem = hash_find (supplemental_hash_table, &page.elem);

  if (elem == NULL)
    return NULL;
  return hash_entry (elem, struct page_elem, elem);
}

/* Remove a page_elem from the supplemental page table. */
void
remove_page_elem (struct hash *supplemental_page_table, struct page_elem *page)
{
  struct hash_elem *elem = hash_delete (supplemental_page_table, &page->elem);
  ASSERT (elem != NULL);
}

/* Lazily allocates a stack page for the processes exceeding one page of 
   memory. */
void 
allocate_stack_page (void *fault_addr)
{
  ASSERT (is_user_vaddr (fault_addr));

  struct thread *t = thread_current ();
  void *rnd_addr = pg_round_down (fault_addr);
  struct hash supplemental_page_table = t->supplemental_page_table;
  struct page_elem *page = create_page_elem_only_vaddr (rnd_addr);
  insert_supplemental_page_entry (&supplemental_page_table, page);

  uint8_t *kpage = frame_table_get_user_page (PAL_ZERO);
  if (kpage == NULL)
    exit_util (KILLED);
  if (!install_page (rnd_addr, kpage, true))
    {
      frame_table_free_user_page (kpage);
      exit_util (KILLED);
    }
}

/* Smart constructor to create a page elem using only a virtual address */
struct page_elem * 
create_page_elem_only_vaddr (void *vaddr)
{
  struct page_elem *page = malloc (sizeof (struct page_elem));
  if (page == NULL)
    return page;

  page->vaddr = vaddr;
  page->rox = false;
  return page;
}

/* Takes a hash_elem and frees the resources associated with the corresponding
   page_elem. */
static void
destroy_hash_elem (struct hash_elem *e, void *aux UNUSED)
{
  struct page_elem *page_elem = hash_entry (e, struct page_elem, elem);
  void *kpage = pagedir_get_page (thread_current ()->pagedir, page_elem->vaddr);

  /* Checking if there is any mapped frame for the current page. If there is a
     mapped frame then free it. */
  if (kpage != NULL)
    {
      if (page_elem->rox)
        {
          file_seek (page_elem->file, page_elem->offset);
          free_frame_for_rox (page_elem);
        }
      else
        frame_table_free_user_page (kpage);
    }
  
  /* Unmaps the page from current thread's page directory and free the struct 
     page_elem since it was malloced on the heap. */
  pagedir_clear_page (thread_current ()->pagedir, page_elem->vaddr);
  free (page_elem);
}

/* Destroys supplemental page table and deallocates all resources. */
void
supplemental_page_table_destroy (struct hash *supplemental_page_table)
{
  hash_destroy (supplemental_page_table, destroy_hash_elem);
}

/* Lazy allocation of a frame from page fault handler. This function must only 
   be called when a file needs to be loaded and not when the staCK needs to be 
   grown. */
void 
allocate_frame (void *fault_addr)
{
  struct thread *t = thread_current ();
  struct hash supplemental_page_table = t->supplemental_page_table;
  struct page_elem page;
  page.vaddr = fault_addr;

  /* Find the page_elem entry in the supplemental page table of the current 
     thread for the fault address. */
  struct hash_elem *elem = hash_find (&supplemental_page_table, &page.elem);
  ASSERT (elem != NULL);
  page = *hash_entry (elem, struct page_elem, elem);
  
  if (page.rox)
    {
      /* Fault occured when trying to read a rox. Get a frame from share table
         rather than frame table. */
      file_seek (page.file, page.offset);
      uint8_t *kpage = get_frame_for_rox (&page);

      /* Add the page to the process's address space. */
      if (!install_page (page.vaddr, kpage, page.writable))
        {
          free_frame_for_rox (&page);
          exit_util (KILLED);
        }

      return;
    }
      
  uint8_t *kpage = pagedir_get_page (t->pagedir, page.vaddr);
  if (kpage == NULL)
    {
      /* Get a new page of memory. */
      kpage = frame_table_get_user_page (0);
      if (kpage == NULL)
        exit_util (KILLED);
      /* Add the page to the process's address space. */
      if (!install_page (page.vaddr, kpage, page.writable))
        {
          frame_table_free_user_page (kpage);
          exit_util (KILLED);
        }
    }

  /* Read the contents of the file into the frame. */
  filesys_acquire ();
  file_seek (page.file, page.offset);
  int bytes_read = file_read (page.file, kpage, page.bytes_read);
  filesys_release ();

  /* Load data into the page. */
  if (bytes_read != (int) page.bytes_read)
    {
      frame_table_free_user_page (kpage);
      exit_util (KILLED);
    }
  memset (kpage + page.bytes_read, 0, page.zero_bytes);
}