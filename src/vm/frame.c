#include "vm/frame.h"
#include "threads/malloc.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"

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

/* Inserts a frame into the frame table. */
static void
insert_frame (void *frame)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  struct frame_elem *frame_elem = malloc (sizeof (struct frame_elem));
  ASSERT (frame != NULL);
  ASSERT (frame_elem != NULL);

  frame_elem->frame = frame;
  frame_elem->swapped = false;
  hash_insert (&frame_table, &frame_elem->elem);
  list_push_back (&all_frames, &frame_elem->all_elem);
}

/* EVicts a frame and puts its contents into the swap table. */
static void
evict_frame (void)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));

  /* Find a frame to evict. */
  struct frame_elem *to_evict = 
      list_entry (list_begin (&all_frames), struct frame_elem, all_elem);

  /* TODO: Remove the frame from the page directories of all the owning threads
     in a thread safe way. */
  

  /* Swap the contents using the swap table. */
  to_evict->swap_id = swap_kpage_in (to_evict->frame);
  to_evict->swapped = true;

  /* Remove the frame from the hash table and the all list. */
  list_remove (&to_evict->all_elem);
  hash_delete (&frame_table, &to_evict->elem);

  palloc_free_page (to_evict->frame);
  to_evict->frame = NULL;
}

/* Deletes a frame from the frame table. */
static void
delete_frame (void *frame)
{
  struct frame_elem frame_elem;
  frame_elem.frame = frame;

  lock_acquire (&frame_table_lock);
  if (hash_find (&frame_table, &frame_elem.elem) == NULL)
    PANIC ("Could not find the requested frame in the frame table");
  
  hash_delete (&frame_table, &frame_elem.elem);
  lock_release (&frame_table_lock);
}

/* Initializes the frame table and its lock. */
void
frame_table_init (void)
{
  lock_init (&frame_table_lock);
  hash_init (&frame_table, frame_hash, frame_less, NULL);
  list_init (&all_frames);
}

/* Gets a user page and puts it in the frame table. */
void *
frame_table_get_user_page (enum palloc_flags flags)
{
  void *page = palloc_get_page (PAL_USER | flags);
  
  lock_acquire (&frame_table_lock);
  if (page == NULL)
    {
      evict_frame ();
      page = palloc_get_page (PAL_USER | flags);
      ASSERT (page != NULL);
    }

  insert_frame (page);
  lock_release (&frame_table_lock);
  return page;
}

/* Frees a user page and removes it from the frame table. */
void
frame_table_free_user_page (void *page)
{
  palloc_free_page (page);
  delete_frame (page);
}

/* Swaps back in a frame that was swapped out. */
void
swap_in_frame (struct frame_elem *frame_elem)
{
  lock_acquire (&frame_table_lock);

  /* We need this check because multiple threads can call this function at the 
     same time but we want to make sure that we do not palloc a frame twice. */
  if (frame_elem->frame != NULL)
    goto done;

  void *page = palloc_get_page (PAL_USER);

  /* Evict a frame if a new frame could not be obtained. */
  if (page == NULL)
    {
      evict_frame ();
      page = palloc_get_page (PAL_USER);
      ASSERT (page != NULL);
    }

  /* Bring in the page from the swap table. Note that this must be done before 
     adding the frame to the threads' page directories as they may see incorrect
     data otherwise. */
  swap_kpage_out (frame_elem->swap_id, page);

  /* Add the frame to all the owning thread's page directories. */
  /* TODO: make this thread safe. */
  frame_elem->frame = page;
  for (struct list_elem *e = list_begin (&frame_elem->owners);
       e != list_end (&frame_elem->owners);
       e = list_next (e))
    {
        struct thread_list_elem *t = 
            list_entry (e, struct thread_list_elem, elem);
        pagedir_set_page (t->t->pagedir, t->vaddr, page, frame_elem->writable);
    }

  /* Insert the frame to the hash table and all list. */
  list_push_back (&all_frames, &frame_elem->all_elem);
  hash_insert (&frame_table, &frame_elem->elem);

done:
  lock_release (&frame_table_lock);
}