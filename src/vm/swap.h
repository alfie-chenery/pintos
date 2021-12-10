#ifndef SWAP_H
#define SWAP_H

#include <devices/block.h>
#include <lib/kernel/hash.h>
#include <threads/thread.h>

struct swap_slot
  {
    size_t index;                   /* Index into the swap table. */
    struct hash_elem elem;          /* To put in the swap table. */
  };

void swap_table_init (void);
void swap_kpage_out (size_t, void *);
size_t swap_kpage_in (void *);
void free_swap_elem(size_t);

#endif /* vm/swap.h */
