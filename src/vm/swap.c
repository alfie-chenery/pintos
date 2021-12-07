#include "swap.h"
#include "vm/frame.h"
#include <lib/kernel/bitmap.h>
#include <threads/vaddr.h>
#include <threads/malloc.h>
#include <threads/palloc.h>

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

struct swap_table 
  {
    struct hash table;          /* Used to initialise the swap table. */
    struct block *swap_block;   /* Block for swap space on disk */
    struct bitmap *used_slots;  /* Tracking used slots */
  };

/* Global lock for the swap table. */
static struct lock swap_table_lock;

/* The global swap table. */
static struct swap_table swap_tbl;

/* Calculates the hash for a swap_elem. */
static unsigned
hash_swap_elem (const struct hash_elem *e, void *aux UNUSED) 
{
  size_t index = hash_entry (e, struct swap_elem, elem)->index;
  return hash_int ((int) index);
}

/* Compares two swap_elem. */
static bool
hash_swap_less (const struct hash_elem *a, 
               const struct hash_elem *b,
               void *aux UNUSED) 
{
    size_t index1 = hash_entry (a, struct swap_elem, elem)->index;
    size_t index2 = hash_entry (b, struct swap_elem, elem)->index;
    return index1 < index2;
}

/* Initialises the swap table lock, hash table and all other members. */
void
swap_table_init (void) 
{
    lock_init (&swap_table_lock);
    hash_init (&swap_tbl.table, hash_swap_elem, hash_swap_less, NULL);
    swap_tbl.swap_block = block_get_role (BLOCK_SWAP);
    swap_tbl.used_slots = bitmap_create 
                    (block_size (swap_tbl.swap_block) / SECTORS_PER_PAGE);
}

/* Writes the kpage to the swap slot indexed by the given index. */
static void
write_to_swap (size_t index, void *kpage) 
{
  ASSERT (!kpage);
  for (int i = 0; i < SECTORS_PER_PAGE; i++) 
    {
      block_sector_t sector = (block_sector_t) (index * SECTORS_PER_PAGE + i);
      block_write 
        (swap_tbl.swap_block, sector, kpage + (i * BLOCK_SECTOR_SIZE));
    }
}

/* Reads the the swap slot indexed by the given slot_index into the 
   given kernel page. */
static void
read_into_kpage (size_t slot_index, void *kpage) 
{
  ASSERT (kpage != NULL);
  for (int i = 0; i < SECTORS_PER_PAGE; i++) 
  {
    block_sector_t sector = (block_sector_t) (slot_index * SECTORS_PER_PAGE + i);
    block_read(swap_tbl.swap_block, sector, kpage + (i * BLOCK_SECTOR_SIZE));
  }
}

/* Move the page stored in the swap slot of the given index into the given 
   kernel page. Updates the swap table to reflect this change. */
void
swap_kpage_out (size_t index, void *kpage) 
{
  lock_acquire (&swap_table_lock);
  /* set the bit at index of used slots bitmaps to false. */
  /* Frees that slot for new pages. */
  bitmap_set (swap_tbl.used_slots, index, false);

  /* Assert the swap tables bit at the index is set to false */
  ASSERT (!bitmap_test (swap_tbl.used_slots, index));

  /* Remove corresponding swap entry from swap table. */
  struct swap_elem del;
  del.index = index;
  ASSERT (!hash_delete (&swap_tbl.table, &del.elem));
  lock_release (&swap_table_lock);

  /* Reading the contents at the index into the page */
  read_into_kpage (index, kpage);
}

/* Creates a new entry in the swap table with the given thread and user page. 
   Copies the given kernel page into a free swap slot. 
   Returns the index of that swap slot. */
size_t
swap_kpage_in (struct thread *parent_thread, void *upage, void *kpage) 
{
  struct swap_elem *elem = malloc (sizeof (struct swap_elem));
  ASSERT (elem);
  
  lock_acquire (&swap_table_lock);
  /* find the first unused slot by searching for first bit set to false */
  size_t idx = bitmap_scan_and_flip (swap_tbl.used_slots, 0, 1, false);
  ASSERT (idx != BITMAP_ERROR);
  
  elem->parent_thread = parent_thread;
  elem->index = idx;
  elem->page = upage;
  
  struct hash_elem *e = hash_insert (&swap_tbl.table, &elem->elem);
  ASSERT (!e);
  
  lock_release (&swap_table_lock);
  write_to_swap (elem->index, kpage);
  /* Free the page */
  palloc_free_page (kpage);
  
  return idx;
}

void
free_swap_elem (size_t index) 
{
  lock_acquire (&swap_table_lock);
  
  bitmap_set (swap_tbl.used_slots, index, false);
  /* Assert the swap tables bit at the index is set to false */
  ASSERT (!bitmap_test (swap_tbl.used_slots, index));
  
  struct swap_elem elem;
  elem.index = index;
  
  struct hash_elem *e = hash_delete (&swap_tbl.table, &elem.elem);
  ASSERT(e);

  lock_release (&swap_table_lock);

  /* Freeing all resources used by the swap_elem */
  struct swap_elem *elem_free = hash_entry (e, struct swap_elem, elem);
  free(elem_free);
}