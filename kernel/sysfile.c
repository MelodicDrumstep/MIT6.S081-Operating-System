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
#include "buf.h"

// #define DEBUG

static struct inode*
create(char *path, short type, short major, short minor);

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
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

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
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

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
// This is HARD LINK
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  // "name" stores the file name
  // "new" is the new file path
  // "old" is the old file path
  struct inode *dp, *ip;
  // dp is the pointer to the directory inode
  // ip is the pointer to the file inode

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
  {
    return -1;
  }
  // Get "old" and "new" from the argument

  begin_op();
  // Notice that we have to enclose every operation with file system
  // to be inside begin_op and end_op
  // to ensure crash safety

  if((ip = namei(old)) == 0)
  {
    end_op();
    return -1;
  }
  // Try to find the inode w.r.t. name old 
  // If failed, [end_op] and return
  // (Remember to end_op!! Otherwise this transaction will continue)

  ilock(ip);
  // Acquire the sleep lock of this inode
  // And read from disk to renew its value if needed

  if(ip->type == T_DIR)
  {
    // If this inode represent a directory
    // release the lock and end_op and return
    // HARD LINK can not be used with directory
    iunlockput(ip);
    // iunlockput will unlock the sleep lock
    // decrease a refcnt
    // and it recnt drop to 0 and link num is 0
    // release the resources of this inode
    end_op();
    return -1;
  }

  ip->nlink++;
  // add the link number
  iupdate(ip);
  // write back the inode in memory to disk
  iunlock(ip);
  // unlock the sleep lock

  if((dp = nameiparent(new, name)) == 0)
  {
    // get the parent directory inode
    // Notice that "name" has not been initialized by sys_link
    // It will be filled by nameiparent to the file name
    goto bad;
  }
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0)
  {
    // dirlink will add (name, inum) to the directory represented by dp
    // So I will store the inode fetched from another path inside this directory
    // This will create hard link because we shared the same inode
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  // error happens
  // nlink-- and write back and drop the refcnt
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Create the path new as a link to the same inode as old.
// This is SOFT LINK
uint64
sys_symlink(void)
{
  char new[MAXPATH], old[MAXPATH];
  // "new" is the new file path
  // "old" is the old file path
  struct inode *ip;
  // we will create a new inode for the link file
  // and assign it to ip.

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
  {
    return -1;
  }
  // Get "old" and "new" from the argument

  begin_op();
  // Notice that we have to enclose every operation with file system
  // to be inside begin_op and end_op
  // to ensure crash safety

  if((ip = namei(new)))
  {
    // This means this file name already exists
    // Which leads to error state
    end_op();
    return -1;
  }

  

  if((ip = create(new, T_SYMLINK, 0, 0)) == 0)
  {
    // Try to Create a new symbolic linked file
    // If failed, [end_op] and return
    // (Remember to end_op!! Otherwise this transaction will continue)
    end_op();
    return -1;
  }
  // ilock(ip);
  // Notice !!!! create has already acquire the lock of this inode
  // SO DO NOT acquire it again!!!

  if(writei(ip, 0, (uint64)old, 0, MAXPATH) < 0)
  {
    // Use "writei" to write the old path to the first bytes inside the first block
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  // Remember to use iunlockput to drop the refcnt and release the lock
  // at the same time
  end_op();

  return 0;
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

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE)
  {
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0)
    {
      end_op();
      return -1;
    }
  } 
  else 
  {
    if((ip = namei(path)) == 0)
    {
      // Get the inode pointer and check if it's valid
      end_op();
      return -1;
    }

    ilock(ip);
    //lock the first ip

    // Follow the symbolic link
    int i;
    // i represent the loop number
    for(i = 0; i < 10; i++)
    {
      if(ip -> type == T_SYMLINK && !(omode & O_NOFOLLOW))
      {
        memset(path, 0, MAXPATH);
        if(readi(ip, 0, (uint64)(path), 0, MAXPATH) <= 0)
        {
          iunlockput(ip);
          end_op();
          return -1;
        }
        // Modify the path to be the path represented by this soft link

        iunlockput(ip);
        // Notice!!! I have to unlock and put the former inode
        // Before fetching the next inode!!!
        // Otherwise it will lead to deadlock or unlock panic

        if((ip = namei(path)) == 0)
        {
          // Get the inode pointer and check if it's valid
          end_op();
          return -1;
        }

        ilock(ip);
        // Remeber to lock the branch new inode here!!
      }
      else
      {
        break;
      }
    }

    // If ip is still symbolic link inode
    // Then I assume it's cyclic
    // then return -1
    if(i == 10 && ip -> type == T_SYMLINK)
    {
      iunlockput(ip);
      end_op();
      //panic("Cyclic symbolic link");
      return -1;
    }
    
    // Cannot ope a directory without read only
    if(ip->type == T_DIR && omode != O_RDONLY)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE)
  {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } 
  else 
  {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip -> type == T_FILE)
  {
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
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
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

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
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

  argaddr(0, &fdarray);
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

//#define DEBUG_MMAP

uint64 
sys_mmap(void)
{
  struct proc * my_proc = myproc();

  int has_allocated = 0;
  // Whether there's enough vma

  int index_vma = 0;

  for(; index_vma < MAX_VMA; index_vma++)
  {
    if(my_proc -> vma[index_vma].used == 0)
    {
      // allocate this vma for this mmap
      has_allocated = 1;
      break;
    }
  }
  if(has_allocated == 0)
  {
    panic("No available vma!");
    return 0xffffffffffffffff;
  }

  //char * mmap(void *addr, size_t length, int prot, int flags,
  //       int fd, off_t offset);
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct file * file;

  // parsing the argument
  argaddr(0, &addr);
  // Notice !! This addr argument is the suggested address by user.
  // I may not use this addr

  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags); 
  argfd(4, &fd, &file);
  argint(5, &offset);

  length = PGROUNDUP(length);
  // Round up to be mutiple of PAGESIZE
  if(length > MAXVA - my_proc -> sz 
  // the process size add length will exceed MAXVA (the maximum of virtual address)
  || (!file -> readable && (prot & PROT_READ))
  // file is not readable and prot require READ
  || ((!file -> writable && (prot & PROT_WRITE)) && (flags == MAP_SHARED))
  // file is not writable , prot require WRITE and it's shared mapping (must be write back)
  || (fd < 0 || fd >= NOFILE)
  // file descriptor should be [0, NOFILE - 1]
  || (file != my_proc -> ofile[fd]))
  // file should be the same as ofile[fd]
  {
    // DEBUGING
    #ifdef DEBUG
      printf("file is not right!\n");
    #endif
    // DEBUGING

    return 0xffffffffffffffff;
  }

  // Fill in the vma
  struct vma * pointer_to_current_vma = &(my_proc -> vma[index_vma]);
  pointer_to_current_vma -> used = 1;
  pointer_to_current_vma -> starting_addr = my_proc -> sz;
  pointer_to_current_vma -> length = length;
  pointer_to_current_vma -> prot = prot;
  pointer_to_current_vma -> flags = flags;
  pointer_to_current_vma -> fd = fd;
  pointer_to_current_vma -> vma_file = file;
  pointer_to_current_vma -> offset = offset;

  my_proc -> sz += length;
  // Now I have a branch new space
  // I have to expand the process size

  filedup(file);
  // filedup will add the refcnt to the file control block
  // This avoids the file to be closed while mmaping

  return pointer_to_current_vma -> starting_addr;
}

uint64 
sys_munmap(void)
{
  struct proc * my_proc = myproc();

  // int munmap(void *addr, size_t length);

  uint64 addr;
  int length;
  // parsing the argument
  argaddr(0, &addr);
  argint(1, &length);

  uint64 end = addr + length;
  // "end" represent the end position of unmap block

  struct vma * pointer_to_vma;

  int has_found = 0;

  int match_start;
  int match_end;
  int vma_start;
  int vma_end;
  // Used in matching

  for(int i = 0; i < MAX_VMA; i++)
  {
    pointer_to_vma = &(my_proc -> vma[i]);
    if(pointer_to_vma -> used == 0)
    {
      continue;
    }

    vma_start = pointer_to_vma -> starting_addr;
    match_start = (addr == vma_start);    
    // Match the start
    vma_end = pointer_to_vma -> starting_addr + pointer_to_vma -> length;
    match_end = (end == vma_end);
    // Match the end

    if((match_start || match_end) && (addr >= vma_start) && (end <= vma_end))
    {
      has_found = 1;

      // DEBUGING
      #ifdef DEBUG
       printf("i is : %d, match_start : %d, match_end : %d\n", i, match_start, match_end);
      #endif
      // DEBUGING

      break;
    }
  }

  if(has_found == 0)
  {

    // DEBUGING
    #ifdef DEBUG
     printf("cannot find any available vma\n");
    #endif
    // DEBUGING

    return -1;
  }

  addr = PGROUNDDOWN(addr);
  length = PGROUNDUP(length);
  // ROUND addr and length

  uint64 pa;

  for(int unmap_addr = addr; unmap_addr < addr + length; unmap_addr += PGSIZE)
  {
    if((pa = walkaddr(my_proc -> pagetable, unmap_addr)) != 0)
    {
      // walkaddr will return the physical address corresponding to 
      // vitual address "unmap_addr"
      // if it's 0, then it's unmapped
      // And I should NOT write to file or unmap the mapping if it's 0


      // DEBUGING
      #ifdef DEBUG
        printf("walkaddr:%d\n", walkaddr(my_proc -> pagetable, unmap_addr));
      #endif
      // DEBUGING

      // Check if I need to write back to the file
      // Notice !! This must happen before doing the "uvmunmap"
      if((pointer_to_vma -> flags & MAP_SHARED)
       && !(pointer_to_vma -> flags & MAP_PRIVATE)
       && (pointer_to_vma -> prot & PROT_WRITE) 
       && (pointer_to_vma -> vma_file -> writable))
       // Notice!! I also have to ensure that the file is writable to me
      {
        if(filewrite(pointer_to_vma -> vma_file, unmap_addr, PGSIZE) < 0)
        {
          
          // DEBUGING
          #ifdef DEBUG
          printf("write file failure\n");
          #endif
          // DEBUGING

          return -1;
        }
      }

      // Get the buffer address and unpin it
      struct buf * mybuf = get_buf_from_data((uchar * )pa);
      // This is important!! I have to set the valid bit to 0 if it's private mapping
      if(pointer_to_vma -> flags & MAP_PRIVATE)
      {
        Invalidate_buf(mybuf);
      }
      bunpin(mybuf);

      uvmunmap(my_proc -> pagetable, unmap_addr, 1, 0);
      // unmap one page at a time, do not kfree the physical page here
      
    }
  }

  if(match_start && match_end)
  {
    // If I unmap the whole memory block
    // release the vma
    pointer_to_vma -> used = 0;
    fileclose(pointer_to_vma -> vma_file);
    // remember to drop the refcnt of the file
  }
  // and modify the vma in other cases
  else if(match_start)
  {
    pointer_to_vma -> starting_addr += length;
    pointer_to_vma -> length -= length;
  }
  else
  {
    pointer_to_vma -> length -= length;
  }
  return 0;
}