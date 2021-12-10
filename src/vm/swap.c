#include "swap.h"
#include "vm/frame.h"
#include <lib/kernel/bitmap.h>
#include <threads/vaddr.h>
#include <threads/malloc.h>
#include <threads/palloc.h>

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* Block for swap space on disk */          
static struct block *swap_block;   

/* Tracking used slots */
static struct bitmap *used_slots;  

/* Global lock for the swap table. */
static struct lock swap_lock;

/* Initialises the swap table lock, hash table and all other members. */
void
swap_table_init (void) 
{
    lock_init (&swap_lock);
    /* hash_init (&swap_table, hash_swap_elem, hash_swap_less, NULL); */
    swap_block = block_get_role (BLOCK_SWAP);
    used_slots = bitmap_create 
                    (block_size (swap_block) / SECTORS_PER_PAGE);
    ASSERT (used_slots != NULL);
}

/* Writes the kpage to the swap slot indexed by the given index. */
static void
write_to_swap (size_t index, void *kpage) 
{
  ASSERT (kpage != NULL);
  for (int i = 0; i < SECTORS_PER_PAGE; i++) 
    {
      block_sector_t sector = (block_sector_t) (index * SECTORS_PER_PAGE + i);
      block_write (swap_block, sector, kpage + (i * BLOCK_SECTOR_SIZE));
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
    block_sector_t sector = 
        (block_sector_t) (slot_index * SECTORS_PER_PAGE + i);
    block_read(swap_block, sector, kpage + (i * BLOCK_SECTOR_SIZE));
  }
}

/* Move the page stored in the swap slot of the given index into the given 
   kernel page. Updates the swap table to reflect this change. */
void
swap_kpage_out (size_t index, void *kpage) 
{
  lock_acquire (&swap_lock);
  /* set the bit at index of used slots bitmaps to false. */
  /* Frees that slot for new pages. */
  bitmap_set (used_slots, index, false);

  /* Assert the swap tables bit at the index is set to false */
  ASSERT (!bitmap_test (used_slots, index));

  /* Reading the contents at the index into the page */
  read_into_kpage (index, kpage);
  lock_release (&swap_lock);
}

/* Creates a new entry in the swap table with the given thread and user page. 
   Copies the given kernel page into a free swap slot. 
   Returns the index of that swap slot. */
size_t
swap_kpage_in (void *kpage) 
{
  struct swap_slot *elem = malloc (sizeof (struct swap_slot));
  ASSERT (elem);

  lock_acquire (&swap_lock);

  /* find the first unused slot by searching for first bit set to false */
  size_t idx = bitmap_scan_and_flip (used_slots, 0, 1, false);

  ASSERT (idx != BITMAP_ERROR);
  
  elem->index = idx;
  
  write_to_swap (elem->index, kpage);
  lock_release (&swap_lock);
  return idx;
}

void
free_swap_elem (size_t index) 
{
  lock_acquire (&swap_lock);
  
  ASSERT (bitmap_test (used_slots, index));
  bitmap_set (used_slots, index, false);
  /* Assert the swap tables bit at the index is set to false */
  ASSERT (!bitmap_test (used_slots, index));
  lock_release (&swap_lock);
}
