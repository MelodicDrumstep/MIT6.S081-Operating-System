// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

struct refcnt
{
  struct spinlock lock;
  int count;
};

struct refcnt count_array[(PHYSTOP - KERNBASE) / PGSIZE];
//This is a count array used for cow-fork 
//to record the number of references to each page

void count_incre(uint64 pa)
{
  int index = (pa - KERNBASE) / PGSIZE;
  acquire(&count_array[index].lock);
  count_array[index].count++;
  release(&count_array[index].lock);
}

void count_init(uint64 pa, int num)
{
  int index = (pa - KERNBASE) / PGSIZE;
  acquire(&count_array[index].lock);
  count_array[index].count = num;
  release(&count_array[index].lock);
}

void count_decre(uint64 pa)
{
  int index = (pa - KERNBASE) / PGSIZE;
  acquire(&count_array[index].lock);
  count_array[index].count--;
  release(&count_array[index].lock);
}

int count_check(uint64 pa, int expected)
{
  int index = (pa - KERNBASE) / PGSIZE;
  acquire(&count_array[index].lock);
  int cnt = count_array[index].count;
  release(&count_array[index].lock);
  return cnt == expected;
}

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

// struct spinlock kmem_master_lock;
// //Newly added : a new lock to protect the kmem array(as a whole)

void
kinit()
{
  for(int i = 0; i < NCPU; i++)
  {
    initlock(&kmem[i].lock, "kmem");
  }
  for(int i = 0; i < (PHYSTOP - KERNBASE) / PGSIZE; i++)
  {
    initlock(&count_array[i].lock, "refcnt");
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    count_init((uint64) p, 1); //set the initial refcount to 1
    kfree(p); //This will decrement the refcount to 0 and free it
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  count_decre((uint64) pa);

  if(count_check((uint64) pa, 0))
  {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    r = (struct run*)pa;

    push_off();
    int cpu_id = cpuid();

    acquire(&kmem[cpu_id].lock);
    r -> next = kmem[cpu_id].freelist;
    kmem[cpu_id].freelist = r; //push_front
    release(&kmem[cpu_id].lock);

    pop_off();
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r)
  {
    kmem[cpu_id].freelist = r -> next;
  }

  release(&kmem[cpu_id].lock);

  if(!r)
  {
    for(int cpu_id2 = cpu_id + 1; cpu_id2 != cpu_id; cpu_id2 = (cpu_id2 + 1) % NCPU)
    {
      acquire(&kmem[cpu_id2].lock);
      r = kmem[cpu_id2].freelist;
      if(r)
      {
        kmem[cpu_id2].freelist = r -> next;
        release(&kmem[cpu_id2].lock);
        break;
      }
      else
      {
        release(&kmem[cpu_id2].lock);
      }
    }
  }

  if(r)
  { 
    memset((char*)r, 5, PGSIZE); // fill with junk
    count_init((uint64) r, 1);
  }

  pop_off();
  return (void*)r;
}

uint64
fmemory_counting(void)
{
  struct run *r;
  uint64 count = 0;

  //pointer r will be used to traverse the freelist
  //and couting the number of free memory run pages
  //stored in "count"

  for(int cpu_id = 0; cpu_id < NCPU; cpu_id++)
  {

    acquire(&kmem[cpu_id].lock);
    //locking before accessing kmem : 
    //many threads may be accessing kmem at the same time!

    for(r = kmem[cpu_id].freelist; r != 0; r = r -> next)
    {
      ++count;
    }

    release(&kmem[cpu_id].lock);
  }

  return count * PGSIZE;
  //count is the number of pages
  //And the return value of this function
  //is the number of bytes
}