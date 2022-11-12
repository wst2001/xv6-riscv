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
  struct spinlock lock[NCPU];
  struct spinlock steallock;
  struct run *freelist[NCPU];
} kmem;

void
kinit()
{
  for (int i = 0; i < NCPU; i ++){
    initlock(&kmem.lock[i], "kmem");
  }
  initlock(&kmem.steallock, "ksteal");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  int cpu_index = 0;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE, cpu_index ++){
    kfree_single_cpu(p, cpu_index % NCPU);
  }
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  // get cpu id
  push_off();
  int cpu_index = cpuid();
  pop_off();
  acquire(&kmem.lock[cpu_index]);
  r->next = kmem.freelist[cpu_index];
  kmem.freelist[cpu_index] = r;
  release(&kmem.lock[cpu_index]);
}

void
kfree_single_cpu(void *pa, int cpu_index)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  acquire(&kmem.lock[cpu_index]);
  r->next = kmem.freelist[cpu_index];
  kmem.freelist[cpu_index] = r;
  release(&kmem.lock[cpu_index]);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // get cpu id
  push_off();
  int cpu_index = cpuid();
  pop_off();

  acquire(&kmem.lock[cpu_index]);
  r = kmem.freelist[cpu_index];
  if(r)
    kmem.freelist[cpu_index] = r->next;
  else{
    for (int i = 0; i < NCPU; i ++){
      if (i == cpu_index)
        continue;
      // CPU i has free page
      acquire(&kmem.lock[i]);
      if (kmem.freelist[i]){
        // steal page
        struct run * next_page = kmem.freelist[i]->next;
        r = kmem.freelist[i];
        kmem.freelist[i] = next_page;
        release(&kmem.lock[i]);
        break;
      }
      release(&kmem.lock[i]);
    }
  }
  release(&kmem.lock[cpu_index]);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
