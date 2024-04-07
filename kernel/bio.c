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

#define BUCKET_SIZE 13

struct BUCKET
{
  struct buf head;
  struct spinlock bucket_lock;
};//BUCKET contains a lock and a header node of struct buf

struct {
  struct spinlock master_lock;
  struct buf buf[NBUF];
  struct BUCKET buckets[BUCKET_SIZE];
} bcache;
//Now bcache contain some buckets of linked list

extern uint ticks;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.master_lock, "bcache");

  for(int i = 0; i < BUCKET_SIZE; i++) 
  {
    initlock(&bcache.buckets[i].bucket_lock, "bucket");
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
  }

  for(b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    initsleeplock(&b -> lock, "buffer");
    b -> ticks = -1;
  }
}

inline int hash(uint dev, uint blockno)
{
  return (dev + blockno) % BUCKET_SIZE;
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int hash_result = hash(dev, blockno);

  acquire(&bcache.buckets[hash_result].bucket_lock);

  // Is the block already cached?

  for(b = bcache.buckets[hash_result].head.next; b != &bcache.buckets[hash_result].head; b = b -> next){
    if(b -> dev == dev && b -> blockno == blockno)
    {
      b -> ticks = ticks;
      b -> refcnt++;
      release(&bcache.buckets[hash_result].bucket_lock);
      acquiresleep(&b -> lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf * to_be_evicted = 0;
  for(b = bcache.buckets[hash_result].head.prev; b != &bcache.buckets[hash_result].head; b = b -> prev)
  {
    if(b -> refcnt == 0 && (to_be_evicted == 0 || (to_be_evicted && b -> ticks < to_be_evicted -> ticks)))
    {
      to_be_evicted = b;
    }
  }

  if(to_be_evicted) //This means I can find the buffer in the double linked list which I hashmap to
  {
    to_be_evicted -> dev = dev;
    to_be_evicted -> blockno = blockno;
    to_be_evicted -> valid = 0; //must be read from disk
    to_be_evicted -> refcnt = 1;
    to_be_evicted -> ticks = ticks;
    release(&bcache.buckets[hash_result].bucket_lock);
    acquiresleep(&to_be_evicted -> lock);
    return to_be_evicted;
  }
  else //This means I cannot find the buffer in the double linked list which I hashmap to
//Then I have to find a buffer in the buf array or evict one from a list
  {
    struct buf * to_be_evicted;
    to_be_evicted = 0;
    for(b = bcache.buf; b < bcache.buf + NBUF; b++)
    {
      if(b -> refcnt == 0 && (to_be_evicted == 0 || (to_be_evicted && b -> ticks < to_be_evicted -> ticks)))
      {
        to_be_evicted = b;
      }
    }

    if(to_be_evicted)
    {
      struct buf * b = to_be_evicted;
      if(b -> ticks != -1)  //This means this buffer is not in any bucket
      {
        int bucket_num = hash(b -> dev, b -> blockno);
        acquire(&bcache.buckets[bucket_num].bucket_lock);
        if(b -> refcnt == 0)
        {
          b -> dev = dev;
          b -> blockno = blockno;
          b -> valid = 0;
          b -> refcnt = 1;
          b -> ticks = ticks;
          
          b -> next -> prev = b -> prev;
          b -> prev -> next = b -> next;
          b -> next = bcache.buckets[hash_result].head.next;
          b -> prev = &bcache.buckets[hash_result].head;
          bcache.buckets[hash_result].head.next -> prev = b;
          bcache.buckets[hash_result].head.next = b;

          release(&bcache.buckets[bucket_num].bucket_lock);
          release(&bcache.buckets[hash_result].bucket_lock);
          acquiresleep(&b -> lock);
          return b;
        }
        else
        {
          release(&bcache.buckets[bucket_num].bucket_lock);
          release(&bcache.buckets[hash_result].bucket_lock);
        }
      }
      else
      {
        acquire(&bcache.master_lock);
        if(b -> refcnt == 0)
        {
          b -> dev = dev;
          b -> blockno = blockno;
          b -> valid = 0;
          b -> refcnt = 1;
          b -> ticks = ticks;

          b -> next = bcache.buckets[hash_result].head.next;
          b -> prev = &bcache.buckets[hash_result].head;
          bcache.buckets[hash_result].head.next -> prev = b;
          bcache.buckets[hash_result].head.next = b;

          release(&bcache.master_lock);
          release(&bcache.buckets[hash_result].bucket_lock);
          acquiresleep(&b -> lock);
          return b;
        }
        else
        {
          release(&bcache.master_lock);
          release(&bcache.buckets[hash_result].bucket_lock);
        }
      }
    }
  }
  bget(dev, blockno);
  //If the program enter here, that means I have to search once again
  //Just use recursion
  panic("bget: no buffers");
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
  if(!holdingsleep(&b -> lock))
  {
    panic("brelse");
  }

  releasesleep(&b -> lock);

  int hash_result = hash(b -> dev, b -> blockno);
  acquire(&bcache.buckets[hash_result].bucket_lock);
  b -> refcnt--;
  release(&bcache.buckets[hash_result].bucket_lock);
}

void
bpin(struct buf *b) 
{
  int hash_result = hash(b -> dev, b -> blockno);
  acquire(&bcache.buckets[hash_result].bucket_lock);
  b -> refcnt++;
  release(&bcache.buckets[hash_result].bucket_lock);
}

void
bunpin(struct buf *b) 
{
  int hash_result = hash(b -> dev, b -> blockno);
  acquire(&bcache.buckets[hash_result].bucket_lock);
  b -> refcnt--;
  release(&bcache.buckets[hash_result].bucket_lock);
}