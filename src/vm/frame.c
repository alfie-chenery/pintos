#include "vm/frame.h"
#include "threads/malloc.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "filesys/file.h"
#include <stdio.h>

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

  /* We check if the beginning of all frames has been accessed recently. If it 
     has then we put it at the back of the list. Otherwise, we evict it. */
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
          ASSERT (t->t->magic == 0xcd6abf4b);
          if (pagedir_is_accessed (t->t->pagedir, t->vaddr))
            is_accessed = true;
          pagedir_set_accessed (t->t->pagedir, t->vaddr, false);
        }

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
  //printf ("DEBUG Evicting %p which is %p\n", to_evict->frame, to_evict->page_elem);

  ASSERT (!to_evict->swapped);
  to_evict->swapped = true;

  /* Remove the frame from the page directories of all the threads that are 
     using it. */
  for (struct list_elem *e = list_begin (&to_evict->owners);
       e != list_end (&to_evict->owners);
       e = list_next (e))
    {
      struct thread_list_elem *t = 
          list_entry (e, struct thread_list_elem, elem);
      ASSERT (t->t->magic == 0xcd6abf4b)
      pagedir_clear_page (t->t->pagedir, t->vaddr);
    }

  /* Swap the contents using the swap table or the file in case of mmap frames. 
     We place this after clearing the page directories of all threads so that if
     they tried to modify the frame then the changes get written to the swap 
     table. */
  if (to_evict->page_elem)
    {
      /* TODO: check for dirty bit. */
      struct page_elem *page_elem = to_evict->page_elem;
      filesys_acquire ();
      file_seek (page_elem->file, page_elem->offset);
      ASSERT (file_write (page_elem->file, to_evict->frame, page_elem->bytes_read) == page_elem->bytes_read);
      filesys_release ();
    }
  else
    to_evict->swap_id = swap_kpage_in (to_evict->frame);

  /* Remove the frame from the hash table and the all list. */
  list_remove (&to_evict->all_elem);
  ASSERT (hash_delete (&frame_table, &to_evict->elem));

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
  //lock_acquire (&frame_table_lock);
  void *page = palloc_get_page (PAL_USER | flags);
  
  lock_acquire (&frame_table_lock);
  if (page == NULL)
    {
      /* TODO: evict frame returns the frame elem so you do not have to palloc again. */
      evict_frame ();
      page = palloc_get_page (PAL_USER | flags);
      ASSERT (page != NULL);
    }

  struct frame_elem *frame_elem = insert_frame (page);
  lock_release (&frame_table_lock);

  frame_elem->writable = writable;
  //lock_release (&frame_table_lock);
  return frame_elem;
}

/* Swaps back in a frame that was swapped out. */
void
swap_in_frame (struct frame_elem *frame_elem)
{
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
        ASSERT (t->t->magic == 0xcd6abf4b);
        ASSERT (pagedir_set_page (t->t->pagedir, t->vaddr, page, frame_elem->writable));
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
  lock_acquire (&frame_table_lock);
  struct thread_list_elem *t = malloc (sizeof (struct thread_list_elem));
  ASSERT (t != NULL);
  if (frame_elem->frame == NULL)
    {
      //ASSERT (frame_elem->swapped);
      swap_in_frame (frame_elem);
    }

  //lock_acquire (&frame_table_lock);
  t->t = thread_current ();
  t->vaddr = vaddr;

  pagedir_set_page (thread_current ()->pagedir, vaddr, 
                    frame_elem->frame, frame_elem->writable);

  //lock_acquire (&frame_table_lock);
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
   all its owners are already dead so we do not need to clear anything from the 
   page directory of any thread. */
void
free_frame_elem (struct frame_elem *frame_elem)
{
  lock_acquire (&frame_table_lock);
  //ASSERT (*((uint8_t *) frame_elem) != 0xcc);

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
      //lock_acquire (&frame_table_lock);
      ASSERT (hash_delete (&frame_table, &frame_elem->elem));
      list_remove (&frame_elem->all_elem);
      //lock_release (&frame_table_lock);
    }

  //printf ("DEBUG Freeing %p which is %p for %i\n", frame_elem->frame, frame_elem->page_elem, thread_tid ());

  /* Freeing the swap space or the frame pointer. */
  if (frame_elem->swapped)
    free_swap_elem (frame_elem->swap_id);
  else
    palloc_free_page (frame_elem->frame);

  free (frame_elem);
  lock_release (&frame_table_lock);
}