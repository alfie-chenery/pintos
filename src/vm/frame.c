#include "vm/frame.h"
#include "threads/malloc.h"

/* The frame table. */
static struct hash frame_table;

/* Lock to control concurrent accesses to the frame table. */
static struct lock frame_table_lock;

/* Calculates the hash for a frame_elem. */
static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
  uint32_t address = hash_entry (e, struct frame_elem, elem)->address;
  return hash_int (address);
}

/* Compares two frame_elem. */
static bool
frame_less (const struct hash_elem *a, 
            const struct hash_elem *b, 
            void *aux UNUSED)
{
  uint32_t address_a = hash_entry (a, struct frame_elem, elem)->address;
  uint32_t address_b = hash_entry (b, struct frame_elem, elem)->address;
  return address_a < address_b;
}

/* Inserts a frame into the frame table. */
static void
insert_frame (uint32_t address)
{
  struct frame_elem *frame = malloc (sizeof (struct frame_elem));
  ASSERT (frame != NULL);

  frame->address = address;
  frame->owner = thread_current ();

  lock_acquire (&frame_table_lock);
  hash_insert (&frame_table, &frame->elem);
  lock_release (&frame_table_lock);
}

/* Deletes a frame from the frame table. */
static void
delete_frame (uint32_t address)
{
  struct frame_elem frame;
  frame.address = address;

  lock_acquire (&frame_table_lock);
  if (hash_find (&frame_table, &frame.elem) == NULL)
    PANIC ("Could not find the requested frame in the frame table");
  
  hash_delete (&frame_table, &frame.elem);
  lock_release (&frame_table_lock);
}

/* Initializes the frame table and its lock. */
void
frame_table_init (void)
{
  lock_init (&frame_table_lock);
  hash_init (&frame_table, frame_hash, frame_less, NULL);
}

/* Gets a user page and puts it in the frame table. */
void *
frame_table_get_user_page (enum palloc_flags flags)
{
  void *page = palloc_get_page (PAL_USER | flags);
  ASSERT (page != NULL);

  insert_frame ((uint32_t) page);
  return page;
}

/* Frees a user page and removes it from the frame table. */
void
frame_table_free_user_page (void *page)
{
  palloc_free_page (page);
  delete_frame ((uint32_t) page);
}