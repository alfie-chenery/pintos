#ifndef __VM_SHARE_H
#define __VM_SHARE_H

#include "filesys/file.h"
#include "vm/page.h"

void share_table_init (void);
void *get_frame_for_rox (struct page_elem *page_elem);
void free_frame_for_rox (struct file *file);

#endif