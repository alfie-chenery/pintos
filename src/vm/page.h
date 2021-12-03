#ifndef __VM_PAGE_H
#define __VM_PAGE_H

#include "lib/kernel/hash.h"

/* Stores an entry in the supplemental page table. */
struct page_elem
  {
    void *vaddr;             /* User virtual address. */
    void *frame;             /* Frame in kernel virtual memory. */
    struct file *file;       /* File pointer to executable file */
    size_t offset;           /* Offset from which to start loading */
    size_t bytes_read;       /* Number of bytes read from file */
    size_t zero_bytes;       /* Number of zero bytes */
    bool writable;           /* File is writable */
    struct hash_elem elem;   /* To create a hash table. */
  };

void supplemental_page_table_init (struct hash *);
void insert_supplemental_page_entry (struct hash *, struct page_elem *);
struct page_elem *create_page_elem (void *, struct file *, size_t, 
                                    size_t, size_t, bool);
bool contains_vaddr (struct hash *, void *);
void allocate_frame (void *);

#endif