// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
struct mem_count {
  struct spinlock lock;
  int count[PHYSTOP / PGSIZE];
} mem_count;

void acquire_mem_count_lock(){
  acquire(&(mem_count.lock));
}

void release_mem_count_lock(){
  release(&(mem_count.lock));
}
void
kinit()
{
  initlock(&kmem.lock, "kmem");
  for (int i = 0; i < PHYSTOP / PGSIZE; i ++){
    initlock(&mem_count.lock, "mem_count");
    mem_count.count[i] = 0;
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
  }

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  uint64 pa_idx = ((uint64)pa) / PGSIZE;

  acquire_mem_count_lock();
  mem_count.count[pa_idx] --;
  if (mem_count.count[pa_idx] > 0){
    release_mem_count_lock();
    return;
  }
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  mem_count.count[pa_idx] = 0;
  release_mem_count_lock();

  
  r = (struct run*)pa;

  acquire(&kmem.lock);    // put into the freelist head
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
  
}

void
kfree_no_lock(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  
  uint64 pa_idx = ((uint64)pa) / PGSIZE;

  mem_count.count[pa_idx] --;
  if (mem_count.count[pa_idx] > 0){
    return;
  }
  mem_count.count[pa_idx] = 0;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run*)pa;

  acquire(&kmem.lock);    // put into the freelist head
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
  
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;    //
  release(&kmem.lock);

  uint pa_idx = (uint64)r / PGSIZE;
  if(r){
    memset((char*)r, 5, PGSIZE); // fill with
    acquire_mem_count_lock();
    mem_count.count[pa_idx] = 1;
    release_mem_count_lock();
  }
  return (void*)r;
}

void *
kalloc_no_lock(void)
{
  struct run *r;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;    //
  release(&kmem.lock);

  uint pa_idx = (uint64)r / PGSIZE;

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with
    mem_count.count[pa_idx] = 1;
  }
  return (void*)r;
}

void
add_mem_count(void *pa){
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  uint64 pa_idx = ((uint64)pa) / PGSIZE;

  acquire_mem_count_lock();
  mem_count.count[pa_idx] ++;
  release_mem_count_lock();
}

void
set_mem_count_no_lock(void *pa, int count){ // the only usage
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  uint64 pa_idx = ((uint64)pa) / PGSIZE;

  mem_count.count[pa_idx] = count;
}

int get_mem_count(void *pa){
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  uint64 pa_idx = ((uint64)pa) / PGSIZE;
  return mem_count.count[pa_idx]; 
}