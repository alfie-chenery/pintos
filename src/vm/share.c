#include "vm/share.h"
#include "lib/kernel/hash.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "userprog/syscall.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include <debug.h>

/* A struct to store read only executables and their allocated frames. */
struct share_elem
  {
    struct file *file;           /* The file pointer. */
    int bytes_read;              /* The bytes read from the file. */
    void *frame;                 /* The allocated frame. */
    int cnt;                     /* Number of threads using this frame. */
    struct hash_elem elem;       /* To craete a hash table. */
  };

/* Hash table to store all the frames which are shared. */
static struct hash share_table;

/* Lock to control concurrent accesses to share table. */
static struct lock share_table_lock;

/* Finds the hash for a share_elem. */
static unsigned
share_elem_hash (const struct hash_elem *e, void *aux UNUSED)
{
  struct share_elem *share_elem = hash_entry (e, struct share_elem, elem);
  return hash_bytes (share_elem->file, sizeof (struct file)) ^
         hash_int (share_elem->bytes_read);
}

/* Compares two share_elem. */
static bool
share_elem_less (const struct hash_elem *a, 
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
  struct file *file_a = hash_entry (a, struct share_elem, elem)->file;
  struct file *file_b = hash_entry (b, struct share_elem, elem)->file;
  int bytes_a = hash_entry (a, struct share_elem, elem)->bytes_read;
  int bytes_b = hash_entry (b, struct share_elem, elem)->bytes_read;

  if (file_a->inode == file_b->inode && file_a->pos == file_b->pos)
    return bytes_a < bytes_b;
  if (file_a->inode == file_b->inode)
    return file_a->pos < file_b->pos;
  return file_a->inode < file_b->inode;
}

/* Initializes the share table and the lock to control it. */
void 
share_table_init (void)
{
  hash_init (&share_table, share_elem_hash, share_elem_less, NULL);
  lock_init (&share_table_lock);
}

/* Checks if an entry alreadys exists for a given rox. Returns NULL if one does
   not exist, otherwise increments its open count by one and returns that. It is
   assumed that the current thread holds share_table_lock before calling this 
   function. */
static void *
get_frame_if_exists (struct page_elem *page_elem)
{
  ASSERT (lock_held_by_current_thread (&share_table_lock));

  struct share_elem share_elem;
  share_elem.file = page_elem->file;
  share_elem.bytes_read = page_elem->bytes_read;

  struct hash_elem *e = hash_find (&share_table, &share_elem.elem);
  if (e == NULL)
    return NULL;

  struct share_elem *ans = hash_entry (e, struct share_elem, elem);
  ans->cnt++;
  return ans->frame;
}

/* Returns a frame for a rox. Loads the file as well if necessary. */
void *
get_frame_for_rox (struct page_elem *page_elem)
{
  struct file *file = page_elem->file;

  /* Ensure that the file is read only. */
  ASSERT (file->deny_write);

  /* We acquire the lock here so that if two thraeds call this function at the 
     same time with the same file *, then we do not malloc a copy twice. */
  lock_acquire (&share_table_lock);

  /* Check if a frame already exists for the rox. */
  void *frame = get_frame_if_exists (page_elem);
  if (frame != NULL)
    goto done;

  /* Create a copy for file so that we are unaffacted if the caller code decides
     to change the contents of the pointer. */
  struct file *file_copy = malloc (sizeof (struct file));
  ASSERT (file_copy != NULL);
  file_copy->inode = file->inode;
  file_copy->pos = file->pos;
  file_copy->deny_write = file->deny_write;

  /* Create a share elem for the current file and allocate a farme for it. */
  struct share_elem *share_elem = malloc (sizeof (struct share_elem));
  ASSERT (share_elem != NULL);
  share_elem->file = file_copy;
  share_elem->cnt = 1;
  share_elem->bytes_read = page_elem->bytes_read;
  frame = share_elem->frame = frame_table_get_user_page (PAL_ZERO);

  /* Insert the share_elem into the hash table. */
  hash_insert (&share_table, &share_elem->elem);

  /* Load the contennts of the file into the frame. */
  filesys_acquire ();
  file_read (file_copy, frame, page_elem->bytes_read);
  file_seek (file_copy, file_tell (file));
  filesys_release ();

done:
  lock_release (&share_table_lock);
  return frame;
}

/* Decrements the open count for the frame associated for a file. Deallocates 
   the frame if no one has it open. */
void
free_frame_for_rox (struct page_elem *page_elem)
{
  struct share_elem find_elem;
  find_elem.file = page_elem->file;
  find_elem.bytes_read = page_elem->bytes_read;

  lock_acquire (&share_table_lock);

  /* Looking for the share_elem in the hash table which corresponds to the
     passed page_elem. */
  struct hash_elem *e = hash_find (&share_table, &find_elem.elem);
  ASSERT (e != NULL);
  struct share_elem *share_elem = hash_entry (e, struct share_elem, elem);

  /* Decrementing open count by 1 and freeing if it has become equal to 0. */
  share_elem->cnt--;
  if (share_elem->cnt == 0)
    {
      hash_delete (&share_table, &share_elem->elem);
      free (share_elem->file);
      frame_table_free_user_page (share_elem->frame);
      free (share_elem);
    }

  lock_release (&share_table_lock);
}
