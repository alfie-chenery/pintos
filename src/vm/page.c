#include "vm/page.h"
#include <debug.h>

/* Calculates the hash for a page_elem. */
static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  void *vaddr = hash_entry (e, struct page_elem, elem)->vaddr;
  return hash_int ((int) vaddr);
}

/* Compares two page_elem. */
static bool
page_less (const struct hash_elem *a, 
           const struct hash_elem *b, 
           void *aux UNUSED)
{
  void *address_a = hash_entry (a, struct page_elem, elem)->vaddr;
  void *address_b = hash_entry (b, struct page_elem, elem)->vaddr;
  return address_a < address_b;
}

/* Initializes a supplemental page table. */
void supplemental_page_table_init (struct hash *supplemental_page_table)
{
  hash_init (supplemental_page_table, page_hash, page_less, NULL);
}
