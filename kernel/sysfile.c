//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

struct vma vma_list[NVMAS];
void vma_init(){
  for (int i = 0; i < NVMAS; i ++){
    initlock(&(vma_list[i].lock), "vma");
  }
}

struct vma* vma_alloc(){
  for (int i = 0; i < NVMAS; i ++){
    acquire(&(vma_list[i].lock));
    if (vma_list[i].length == 0){
      return &(vma_list[i]);
    }
    release(&(vma_list[i].lock));
  }
  panic("no enough vma");
}

struct vma * vma_search(pte_t va){
  struct vma * pv = myproc()->vma;
  if (pv == 0){
    return 0;
  }
  while (pv){
    // acquire(&(pv->lock));
    if ((pv->start <= va) && (va < pv->end)){
      return pv;
    }
    // release(&(pv->lock));
    pv = pv->next;
  }
  return 0;
}
int mmap_handler(pte_t va, uint64 scause){
  struct proc * p = myproc();
  struct vma * pv;
  if ((pv = vma_search(va)) == 0){
    goto bad;
  }
  if (scause == 13 && !(pv->permission & PTE_R)){
    goto bad;
  }
  if (scause == 15 && !(pv->permission & PTE_W)){
    goto bad;
  }
  struct file * f = pv->file;
  va = PGROUNDDOWN(va);
  char * mem;
  if ((mem = kalloc()) == 0){
    kfree(mem);
    goto bad;
  }
  memset(mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, pv->permission) < 0){
    goto bad;
  }
  ilock(f->ip);
  if (readi(f->ip, 0, (uint64)mem, (pv->offset + va - pv->start), PGSIZE) < 0){
    goto bad;
  }
  iunlock(f->ip);
  // release(&(pv->lock));
  return 0;
bad:
  // release(&(pv->lock));
  return -1;
}
uint64
sys_mmap(void){
  uint64 addr, fd;
  int length, prot, flags, offset;
  if (argaddr(0, &addr) < 0 || argint(1, &length) < 0 || argint(2, &prot) < 0 || argint(3, &flags) < 0 || argaddr(4, &fd) < 0 || argint(5, &offset) < 0){
    return -1;
  }
  struct proc *p = myproc();
  struct vma * v = vma_alloc();
  struct file * f = p->ofile[fd];
  int pte_flag = PTE_U;
  if (prot & PROT_READ){
    if (!(f->readable)){
      release(&(v->lock));
      return -1;
    }
    pte_flag |= PTE_R;
  }
  if (prot & PROT_WRITE){
    if (!(f->writable) && !(flags & MAP_PRIVATE)){
      release(&(v->lock));
      return -1;
    }
    pte_flag |= PTE_W;
  }
  v->length = length;
  v->permission = pte_flag;
  v->offset = offset;
  v->flags = flags;
  v->file = f;
  filedup(f);

  struct vma * pv = p->vma;
  if (pv == 0){
    v->start = VMA_START;
    v->end = v->start + length;
    p->vma = v;
  }
  else{
    while (pv->next){
      pv = pv->next;
    }
    v->start = PGROUNDUP(pv->end);
    v->end = v->start + length;
    pv->next = v;
    v->next = 0;
  }
  addr = v->start;
  release(&(v->lock));
  return addr;
}

// int writeback(struct vma * v, uint64 addr, int length){
//   if ((v->flags & MAP_PRIVATE) || !(v->permission & PTE_W)){
//     return 0;
//   }
//   if (addr % PGSIZE != 0){
//     panic("unmap: no aligned");
//   }
//   if (filewrite(v->file, addr, length) < 0){
//     return -1;
//   }
//   return 0;
// }

void
writeback(struct vma* v, uint64 addr, int n)
{
  if(!(v->permission & PTE_W) || (v->flags & MAP_PRIVATE)) // no need to writeback
    return;

  if((addr % PGSIZE) != 0)
    panic("unmap: not aligned");

  struct file* f = v->file;

  int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
  int i = 0;
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;
    begin_op();
    ilock(f->ip);
    int r = writei(f->ip, 1, addr + i, v->offset + addr - v->start + i, n1);
    iunlock(f->ip);
    end_op();
    i += r;
  }
}

int apply_munmap(uint64 addr, int length){
  struct proc * p = myproc();
  struct vma * pv = p->vma;
  struct vma * v;
  if ((v = vma_search(addr)) == 0){
    goto bad;
  }
  if (addr != v->start && addr + length != v->end){
    panic("munmap middle");
  }
  uint64 va;
  for (va = PGROUNDDOWN(addr); va <= addr + length; va += PGSIZE){
    if (walkaddr(p->pagetable, va) != 0){
      writeback(v, va, PGSIZE);
      uvmunmap(p->pagetable, va, 1, 1);
    }
  }
  
  
  
  if (addr == v->start && addr + length == v->end){
    fileclose(v->file);
    if (v == pv){
      p->vma = v->next;
    }
    else{
      struct vma * pre = pv;
      while (pre->next && pre->next != v){
        pre = pre->next;
      }
      pre->next = pv->next;
    }
    acquire(&(v->lock));
    v->length = 0;
    v->next = 0;
    v->flags = 0;
    v->permission = 0;
    release(&(v->lock));

  }
  else if (addr == v->start) {
    v->start = addr + length;
    v->length -= length;
    v->offset += length;
  }
  else if (addr + length == v->end){
    v->end = addr;
    v->length -= length;
  }
  return 0;
bad:
  return -1;
}

uint64
sys_munmap(void){
  uint64 addr;
  int length;
  if (argaddr(0, &addr) < 0 || argint(1, &length) < 0){
    return -1;
  }
  return apply_munmap(addr, length);
}