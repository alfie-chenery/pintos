#include "vm/frame.h"
#include "threads/malloc.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "filesys/file.h"
#include "threads/interrupt.h"

/* The frame table. */
static struct hash frame_table;

/* Lock to control concurrent accesses to the frame table. */
static struct lock frame_table_lock;

/* List to store all the allocated frames. */
static struct list all_frames;

/* Calculates the hash for a frame_elem. */
static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
  void *frame = hash_entry (e, struct frame_elem, elem)->frame;
  return hash_int ((int) frame);
}

/* Compares two frame_elem. */
static bool
frame_less (const struct hash_elem *a, 
            const struct hash_elem *b, 
            void *aux UNUSED)
{
  void *frame_a = hash_entry (a, struct frame_elem, elem)->frame;
  void *frame_b = hash_entry (b, struct frame_elem, elem)->frame;
  return frame_a < frame_b;
}

/* Creates a frame_elem for a pointer to a frame and inserts it into the frame 
   table and all_frames list. Returns the created frame. */
static struct frame_elem *
insert_frame (void *frame)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  struct frame_elem *frame_elem = malloc (sizeof (struct frame_elem));
  ASSERT (frame != NULL);
  ASSERT (frame_elem != NULL);

  /* Setting the fields of the frame_elem. */
  frame_elem->frame = frame;
  frame_elem->swapped = false;
  frame_elem->page_elem = NULL;
  list_init (&frame_elem->owners);

  /* Adding the frame to the frame table and all_frames list. */
  ASSERT (hash_insert (&frame_table, &frame_elem->elem) == NULL);
  list_push_back (&all_frames, &frame_elem->all_elem); 

  return frame_elem;
}

/* Chooses a frame to evict using second chance algorithm. */
static struct frame_elem *
choose_frame_to_evict (void)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  /* We check if the frame at the beginning of all frames list has been accessed
     recently by checking the accessed bit of all the threads that use that 
     frame. If it has then we put it at the back of the list. Otherwise, we 
     evict it. */
  while (true)
    {
      struct frame_elem *frame_elem = 
          list_entry (list_begin (&all_frames), struct frame_elem, all_elem);
      bool is_accessed = false;

      /* Iterate through the owners and check if any of them have accesses the
         frame recently. */
      for (struct list_elem *e = list_begin (&frame_elem->owners);
           e != list_end (&frame_elem->owners);
           e = list_next (e))
        {
          struct thread_list_elem *t = 
              list_entry (e, struct thread_list_elem, elem);
          if (pagedir_is_accessed (t->t->pagedir, t->vaddr))
            is_accessed = true;

          /* We clear the accessed bit so that when we reach this frame next 
             time, it has a chance of being evicted. */
          pagedir_set_accessed (t->t->pagedir, t->vaddr, false);
        }

      /* Return this frame if it has not been accessed since we allocated it or
         last considered it for eviction. */
      if (!is_accessed)
        return frame_elem;

      /* Remove the frame from the front and move it to the back. */
      list_remove (&frame_elem->all_elem);
      list_push_back (&all_frames, &frame_elem->all_elem);
    }

  /* We should never reach here. */
  NOT_REACHED ();
}

/* Evicts a frame and puts its contents into the swap table. The frame to be
   evicted is chosen using the second chance algorithm. */
static void
evict_frame (void)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  /* Find a frame to evict. */
  struct frame_elem *to_evict = choose_frame_to_evict ();

  ASSERT (!to_evict->swapped);
  to_evict->swapped = true;

  bool is_dirty = false;

  /* Remove the frame from the page directories of all the threads that are 
     using it. */
  for (struct list_elem *e = list_begin (&to_evict->owners);
       e != list_end (&to_evict->owners);
       e = list_next (e))
    {
      struct thread_list_elem *t = 
          list_entry (e, struct thread_list_elem, elem);

      /* If the current frame is for a frame_elem, then we need to check if the
         dirty bit has been set. */
      if (to_evict->page_elem)
        {
          /* We need to disable the interrupts here so that the thread cannot 
             modify the memory in between us checking if it is dirty and 
             marking the page as not present in the page directory. */
          enum intr_level old_level = intr_disable ();
          is_dirty = is_dirty || pagedir_is_dirty (t->t->pagedir, t->vaddr);
          pagedir_clear_page (t->t->pagedir, t->vaddr);
          intr_set_level (old_level);
        }
      else
        pagedir_clear_page (t->t->pagedir, t->vaddr);
    }

  /* Swap the contents using the swap table or the file in case of mmap frames. 
     We place this after clearing the page directories of all threads so that if
     they tried to modify the frame then the changes get written to the swap 
     table. */
  if (to_evict->page_elem && is_dirty)
    {
      struct page_elem *page_elem = to_evict->page_elem;
      filesys_acquire ();
      file_seek (page_elem->file, page_elem->offset);
      file_write (page_elem->file, to_evict->frame, page_elem->bytes_read);
      filesys_release ();
    }
  else
    to_evict->swap_id = swap_kpage_in (to_evict->frame);

  /* Remove the frame from the hash table and the all list. */
  list_remove (&to_evict->all_elem);
  struct hash_elem *e = hash_delete (&frame_table, &to_evict->elem);
  ASSERT (e);

  /* Free the physical memory of the frame and mark that in the frame_elem. */
  palloc_free_page (to_evict->frame);
  to_evict->frame = NULL;
}

/* Initializes the frame table and its lock. */
void
frame_table_init (void)
{
  lock_init (&frame_table_lock);
  hash_init (&frame_table, frame_hash, frame_less, NULL);
  list_init (&all_frames);
}

/* Gets a user page and puts it in the frame table. Returns the frame_elem that 
   was created. */
struct frame_elem *
frame_table_get_user_page (enum palloc_flags flags, bool writable)
{
  void *page = palloc_get_page (PAL_USER | flags);
  
  lock_acquire (&frame_table_lock);
  if (page == NULL)
    {
      evict_frame ();
      page = palloc_get_page (PAL_USER | flags);
      ASSERT (page != NULL);
    }

  struct frame_elem *frame_elem = insert_frame (page);
  lock_release (&frame_table_lock);

  frame_elem->writable = writable;
  return frame_elem;
}

/* Swaps back in a frame that was swapped out. */
void
swap_in_frame (struct frame_elem *frame_elem)
{
  /* This function can be called externalls as well as internally by another 
     function (which would already hold the frame table lock). Hence we need to
     be careful that we do not acquire the lock twice. */
  bool locked = false;
  if (!lock_held_by_current_thread (&frame_table_lock))
    {
      locked = true;
      lock_acquire (&frame_table_lock);
    }

  /* We need this check because multiple threads can call this function at the 
     same time but we want to make sure that we do not palloc a frame twice. */
  if (frame_elem->frame != NULL)
    goto done;

  void *page = palloc_get_page (PAL_USER | PAL_ZERO);

  /* Evict a frame if a new frame could not be obtained. */
  if (page == NULL)
    {
      evict_frame ();
      page = palloc_get_page (PAL_USER | PAL_ZERO);
      ASSERT (page != NULL);
    }

  /* Bring in the page from the swap table or the file system. Note that this 
     must be done before adding the frame to the threads' page directories as 
     they may see incorrect data otherwise. */
  if (frame_elem->page_elem)
    {
      ASSERT (list_size (&frame_elem->owners) == 1);
      struct page_elem *page_elem = frame_elem->page_elem;
      filesys_acquire ();
      file_seek (page_elem->file, page_elem->offset);
      file_read (page_elem->file, page, page_elem->bytes_read);
      filesys_release ();
    }
  else
    swap_kpage_out (frame_elem->swap_id, page);

  /* Mark that the frame is no longer swapped and update the frame. */
  frame_elem->swapped = false;
  frame_elem->frame = page;

  /* Insert the frame to the hash table and all list. */
  list_push_back (&all_frames, &frame_elem->all_elem);
  ASSERT (hash_insert (&frame_table, &frame_elem->elem) == NULL);

  /* Add the frame to all the owning thread's page directories. */
  for (struct list_elem *e = list_begin (&frame_elem->owners);
       e != list_end (&frame_elem->owners);
       e = list_next (e))
    {
        struct thread_list_elem *t = 
            list_entry (e, struct thread_list_elem, elem);
        pagedir_set_page (t->t->pagedir, t->vaddr, page, frame_elem->writable);
    }

done:
  if (locked)
    lock_release (&frame_table_lock);
}

/* Adds the running thread to the list of owners of a frame_elem and installs 
   the page into the thread's page directory. */
void
add_owner (struct frame_elem *frame_elem, void *vaddr)
{
  struct thread_list_elem *t = malloc (sizeof (struct thread_list_elem));
  ASSERT (t != NULL);

  /* We acquire the lock here so that the frame does not get swapped in twice if
     two threads call this function twice. */
  lock_acquire (&frame_table_lock);
  if (frame_elem->frame == NULL)
      swap_in_frame (frame_elem);

  t->t = thread_current ();
  t->vaddr = vaddr;

  pagedir_set_page (thread_current ()->pagedir, vaddr, 
                    frame_elem->frame, frame_elem->writable);

  list_push_back (&frame_elem->owners, &t->elem);
  lock_release (&frame_table_lock);
}

/* Removes the running thread from the list of owners of the frame_elem. */
void
remove_owner (struct frame_elem *frame_elem)
{
  lock_acquire (&frame_table_lock);
  for (struct list_elem *e = list_begin (&frame_elem->owners);
       e != list_end (&frame_elem->owners);
       e = list_next (e))
    {
      struct thread_list_elem *t = 
          list_entry (e, struct thread_list_elem, elem);
      if (t->t == thread_current ())
        {
          list_remove (&t->elem);
          free (t);
          lock_release (&frame_table_lock);
          return;
        }
    }

  /* We should not reach here as this means the running thread did not belong to
     the owners of the frame. */
  NOT_REACHED ();
}

/* Frees a frame_elem and the frame pointer stored inside it. It is assumed that
   all its owners are already dead or in the process of exiting so we do not 
   need to clear anything from the page directory of any thread. */
void
free_frame_elem (struct frame_elem *frame_elem)
{
  lock_acquire (&frame_table_lock);

  /* Free the list of owners of the thread. */
  ASSERT (list_size (&frame_elem->owners) == 1);
  while (!list_empty (&frame_elem->owners))
    {
      struct list_elem *e = list_begin (&frame_elem->owners);
      struct thread_list_elem *t = 
          list_entry (e, struct thread_list_elem, elem);
      list_remove (&t->elem);
      free (t);
    } 

  /* Remove from frame table and all frames if the frame was not currently 
     swapped. */
  if (!frame_elem->swapped)
    {
      ASSERT (frame_elem->frame != NULL);
      struct hash_elem *e = hash_delete (&frame_table, &frame_elem->elem);
      ASSERT (e);
      list_remove (&frame_elem->all_elem);
    }

  /* Freeing the swap space or the frame pointer. */
  if (frame_elem->swapped)
    free_swap_elem (frame_elem->swap_id);
  else
    palloc_free_page (frame_elem->frame);

  free (frame_elem);
  lock_release (&frame_table_lock);
}