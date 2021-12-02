#ifndef __VM_PAGE_H
#define __VM_PAGE_H

#include "lib/kernel/hash.h"

/* Stores an entry in the supplemental page table. */
struct page_elem
  {
    void *vaddr;             /* User virtual address. */
    void *frame;             /* Frame in kernel virtual memory. */
    struct hash_elem elem;   /* To create a hash table. */
  };

void supplemental_page_table_init (struct hash *page_table);

#endif