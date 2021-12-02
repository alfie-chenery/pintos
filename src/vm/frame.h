#ifndef __VM_FRAME_H
#define __VM_FRAME_H

#include "threads/thread.h"
#include "lib/kernel/hash.h"
#include "threads/palloc.h"

/* Stores an entry in the frame table. */
struct frame_elem 
  {
    void *frame;                /* Pointer to frame in memory. */
    struct thread *owner;       /* The thread which owns the frame. */
    struct hash_elem elem;      /* To add this in a hash table. */
  };

void frame_table_init (void);
void *frame_table_get_user_page (enum palloc_flags flags);
void frame_table_free_user_page (void *page);

#endif