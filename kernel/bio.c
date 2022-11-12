// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock[NBUCKETS];
  struct spinlock steallock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKETS];
} bcache;

void
binit(void)
{
  struct buf *b;
  for (int i = 0; i < NBUCKETS; i ++){
    initlock(&bcache.lock[i], "bcache");
  }
  initlock(&bcache.steallock, "bsteal");

  // Create linked list of buffers
  for (int i = 0; i < NBUCKETS; i ++){
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  int bcache_id = 0;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->time_stamp = ticks;

    b->next = bcache.head[bcache_id].next;
    b->prev = &bcache.head[bcache_id];
    initsleeplock(&b->lock, "buffer");
    bcache.head[bcache_id].next->prev = b;
    bcache.head[bcache_id].next = b;
    bcache_id = (bcache_id + 1) % NBUCKETS;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int bcache_id = blockno % NBUCKETS;
  acquire(&bcache.lock[bcache_id]);

  // Is the block already cached?
  for(b = bcache.head[bcache_id].next; b != &bcache.head[bcache_id]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->time_stamp = ticks;
      b->refcnt++;
      release(&bcache.lock[bcache_id]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  int min_time_stamp = 0x7fffffff;
  struct buf * replace_buf = 0;
  for(b = bcache.head[bcache_id].prev; b != &bcache.head[bcache_id]; b = b->prev){
    if(b->refcnt == 0 && b->time_stamp < min_time_stamp) {
      replace_buf = b;
      min_time_stamp = b->time_stamp;
    }
  }
  if (replace_buf){
    goto found;
  }

  // steal cache
  acquire(&bcache.steallock);
  refind:
  for (b = bcache.buf; b < bcache.buf + NBUF; b ++){
    if(b->refcnt == 0 && b->time_stamp < min_time_stamp) {
      replace_buf = b;
      min_time_stamp = b->time_stamp;
    }
  }

  if (replace_buf){
    int replace_id = replace_buf->blockno % NBUCKETS;
    acquire(&bcache.lock[replace_id]);
    if (replace_buf->refcnt != 0){
      release(&bcache.lock[replace_id]);
      goto refind;
    }
    // insert and remove
    struct buf * prevbuf = &bcache.head[bcache_id];
    struct buf * nextbuf = bcache.head[bcache_id].next;
    prevbuf->next = replace_buf;
    nextbuf->prev = replace_buf;
    replace_buf->prev->next = replace_buf->next;
    replace_buf->next->prev = replace_buf->prev;
    replace_buf->prev = prevbuf;
    replace_buf->next = nextbuf;

    release(&bcache.lock[replace_id]);
    release(&bcache.steallock);
    goto found;
  }
  else{
    panic("bget: no buffers");
  }
  found:
  replace_buf->dev = dev;
  replace_buf->blockno = blockno;
  replace_buf->valid = 0;
  replace_buf->refcnt = 1;
  release(&bcache.lock[bcache_id]);
  acquiresleep(&replace_buf->lock);
  return replace_buf;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bcache_id = b->blockno % NBUCKETS;
  acquire(&bcache.lock[bcache_id]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head[bcache_id].next;
    b->prev = &bcache.head[bcache_id];
    bcache.head[bcache_id].next->prev = b;
    bcache.head[bcache_id].next = b;
  }
  
  release(&bcache.lock[bcache_id]);
}

void
bpin(struct buf *b) {
  int bcache_id = b->blockno % NBUCKETS;
  acquire(&bcache.lock[bcache_id]);
  b->refcnt++;
  release(&bcache.lock[bcache_id]);
}

void
bunpin(struct buf *b) {
  int bcache_id = b->blockno % NBUCKETS;
  acquire(&bcache.lock[bcache_id]);
  b->refcnt--;
  release(&bcache.lock[bcache_id]);
}


