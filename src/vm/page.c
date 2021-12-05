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
  page->frame = NULL;
  page->file = file;
  page->offset = offset;
  page->bytes_read = bytes_read;
  page->zero_bytes = zero_bytes;
  page->writable = writable;
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

/* Lazily allocates a stack page for the processes exceeding one page of memory. */
void 
allocate_stack_page (struct thread *t, void *fault_addr)
{
  ASSERT (is_user_vaddr (fault_addr));

  void *rnd_addr = pg_round_down (fault_addr);
  struct hash supplemental_page_table = t->supplemental_page_table;
  struct page_elem *page = create_page_elem_only_vaddr (rnd_addr);
  insert_supplemental_page_entry(&supplemental_page_table, page);

  uint8_t *kpage = frame_table_get_user_page (PAL_ZERO);
  if (kpage == NULL)
    exit_util (KILLED);
  if (!install_page (rnd_addr, kpage, true))
    frame_table_free_user_page (kpage);
}

/* Smart constructor to create a page elem using only a virtual address */
struct page_elem * 
create_page_elem_only_vaddr (void *vaddr)
{
  struct page_elem *page = malloc (sizeof (struct page_elem));
  if (page == NULL)
    return page;

  page->vaddr = vaddr;
  return page;
}

/* Lazy allocation of a frame from page fault handler */
void 
allocate_frame (void *fault_addr)
{
  // 
  struct thread *t = thread_current ();
  struct hash supplemental_page_table = t->supplemental_page_table;
  struct page_elem page;
  page.vaddr = fault_addr;

  // 
  struct hash_elem *elem = hash_find (&supplemental_page_table, &page.elem);
  ASSERT (elem != NULL);
  page = *hash_entry (elem, struct page_elem, elem);

  // 
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

  (&page)->frame = kpage;

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
  memset (kpage + page.bytes_read, 0, page.bytes_read);
  
  // void *kpage = frame_table_get_user_page (PAL_ZERO);
  // if (kpage == NULL)
  //   exit_util (KILLED);

  // // 
  // if (!install_page (page.vaddr, kpage, page.writable))
  //   {
  //     frame_table_free_user_page (kpage);
  //     exit_util (KILLED);
  //   }

  // // 
  // if (file_read (page.file, kpage, page.bytes_read) != (int) page.bytes_read)
  //   {
  //     frame_table_free_user_page (kpage);
  //     exit_util (KILLED);
  //   }
}