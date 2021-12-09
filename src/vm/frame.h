#ifndef __VM_FRAME_H
#define __VM_FRAME_H

#include "threads/thread.h"
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include "threads/palloc.h"

/* A struct to create a list of threads who have a frame in their page \
   directory and the user address where they have it. */
struct thread_list_elem
  {
    struct thread *t;           /* The thread. */
    void *vaddr;                /* User virtual address. */
    struct list_elem elem;      /* To create a list. */
  };

/* Stores an entry in the frame table. */
struct frame_elem 
  {
    void *frame;                /* Pointer to frame in memory. */
    bool swapped;               /* If the frame is currently swapped. */
    size_t swap_id;             /* The swap id if it is swapped. */
    struct list owners;         /* The threads which own the list. */ 
    bool writable;              /* If the frame is writable. */
    struct hash_elem elem;      /* To add this in a hash table. */
    struct list_elem all_elem;  /* For creating list of all frames. */
  };

void frame_table_init (void);
struct frame_elem *frame_table_get_user_page (enum palloc_flags, bool writable);
void swap_in_frame (struct frame_elem *frame_elem);
void add_owner (struct frame_elem *frame_elem, void *vaddr);
void free_frame_elem (struct frame_elem *frame_elem);

#endif