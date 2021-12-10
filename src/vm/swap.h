#ifndef SWAP_H
#define SWAP_H

#include <threads/thread.h>

void swap_table_init (void);
void swap_kpage_out (size_t, void *);
size_t swap_kpage_in (void *); 
void free_swap_elem (size_t);

#endif /* vm/swap.h */