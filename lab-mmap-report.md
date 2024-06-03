这个 lab 是写一个 `mmap` 和 `munmap` 系统调用。 我之前写过只使用 `sbrk` 的 `malloc`, 写完 `mmap` 之后对内存分配的理解就会更深了。

# 前置工作

首先就是注册系统调用。原型写成这样:

```c
char * mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
int munmap(void *addr, size_t length);
```

然后其他地方注册好。 这步完成之后 `mmaptest` 就可以编译了。

## mmap 原型介绍

这里 `mmap` 的第一次参数是用户建议的内存地址， 本次 lab 中默认不使用这个参数(即 `addr = 0`)

然后 `mmap` 返回分配的头地址(虚拟地址)。 如果失败， 返回 `0xffffffffffffffff`.

`length` 就是内存块的长度， 单位为 Byte.

`prot` 和 `flag` 都是一些配置参数， 我们本次 lab 用到的有这些:

```c
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
// MAP_SHARED means that modifications to the mapped memory should
// Always be written back to file
// It's OK if process that map the same MAP_SHARED files DO NOT
// shared physical pages
#define MAP_PRIVATE     0x02
// MAP_PRIVATE means that we should not write back on modifying
```

`fd` 是打开的文件描述符。 `offset` 是文件偏置， 这里可以假设 `offset = 0`.

## munmap 原型介绍

`munmap` 会释放 `mmap` 申请的那块内存。 如果标记为 `MAP_SHARED` 而且有修改过， 那么要记得写回到文件。 

然后， `munmap` 可以作用于这块内存的一部分。 但是我们规定， 以下情况至少满足其一:

+ `munmap` 指定的内存区域从 `mmap` 申请内存的头部开始。

+ `munmap` 指定的内存区域从 `mmap` 申请内存的尾部结束。

## struct file

我们表示一个文件的结构体是这样实现的:

```c
struct file {
#ifdef LAB_NET
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_SOCK } type;
#else
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
#endif
  int ref; 
  // reference count

  char readable;
  char writable;
  // permissions
  struct pipe *pipe; 
  // a pointer to pipe, only apply to file type FD_PIPE

  struct inode *ip;  
  // a pointer to inode, only apply to file type FD_INODE and FD_DEVICE

#ifdef LAB_NET
  struct sock *sock; // FD_SOCK
#endif
  uint off;          
  // offset, only apply to file type FD_INODE
  short major;       
  // the device number, only apply to file type FD_DEVICE
};
```

## file descriptor 介绍

我们可以用上述 `file` 结构体来描述一个文件的特征， 但有时候传这个结构体很麻烦， 所以我们可以用 `file descriptor` 来加一层抽象。

首先， 我们每个进程都会维护一个 `struct file array` 来表示当前进程打开的所有文件。 

```c
struct proc
{
//...
    struct file *ofile[NOFILE];  // Open files
//...
};
```

那么其实， 我可以直接用 `index` 来描述一个进程打开的某个文件， 这个 `index` 被我们叫做 `file descriptor`（文件描述符）。

之后， 我们的 `read / write` 等函数都可以传入文件描述符实现一级封装。

# 开工!

## 定义我的 VMA

任务提示中说

```
Keep track of what mmap has mapped for each process. Define a structure corresponding to the VMA (virtual memory area) described in Lecture 15, recording the address, length, permissions, file, etc. for a virtual memory range created by mmap. Since the xv6 kernel doesn't have a memory allocator in the kernel, it's OK to declare a fixed-size array of VMAs and allocate from that array as needed. A size of 16 should be sufficient.
```

这个很好理解， 我直接给 PCB (process control block) 加一个区域存一堆 VMA 就可以了。 

```c
struct vma
{
  int used;
  uint64 starting_addr;
  int length;
  int prot;
  int flags;
  char filename[MAXPATH];
  int fd;
  int offset;
  struct file * vma_file;
  // one vma deals with one "mmap"
  // It will record the starting address, length of this memory block
  // the permissions, and the backup file name & file descriptor & file offset
  // & file control block pointer
  // used detect whether this vma is being used
  // this is for allocate new valid vma
};

// Per-process state
struct proc 
{
  //...
  struct vma vma[MAX_VMA];
  //...
}
```

然后进程分配的时候做一下初始化:

```c
// initialize all vma in this process to be unused
for(int i = 0; i < MAX_VMA; i++)
{
  p -> vma[i].used = 0;
}
```

## mmap

然后开始写 `mmap`. 大致思路就是， 寻找空闲 `VMA`, 做安全检查， 然后填充 `VMA`. 这里不使用用户提供的 `addr suggestion`, 直接分配在进程使用的最大空间之后。

这个有一个问题是：我把这部分内存块映射到虚拟内存的哪个位置？ 

我们可以从 `kernel/memlayout.h` 中看到 xv6 的虚拟内存布局:

```
// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   USYSCALL (shared with kernel)
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
```

通常情况下, 我们应该把 `mmap` 区域放在一个进程的堆和栈之间， 但是这里我们的进程没有维护便利的数据结构来找到栈和堆之间满足一定大小的一块内存空间。 所以这里我这样实现: 直接把这块内存空间接在当前进程使用的最大地址 (虚拟地址) 的后面。 这样可能会产生碎片， 但安全性得到了保障。

所以就这样实现:

```c
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
    return -1;
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
  || ((!file -> writable && (prot & PROT_WRITE)) && (flags == MAP_SHARED)))
  // file is not writable , prot require WRITE and it's shared mapping (must be write back)
  {
    return -1;
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
```

现在跑 `mmaptest` 会发现， page fault 之后没有 handler 来处理， 所以会崩溃:

```
$ mmaptest
mmap_test starting
test mmap f
usertrap(): unexpected scause 0x000000000000000d pid=3
            sepc=0x0000000000000074 stval=0x0000000000005000
panic: uvmunmap: not mapped
backtrace:
0x0000000080006afc
0x00000000800009ce
0x0000000080000c56
0x00000000800012a2
```

接下来需要写 page fault 的 handler.

## page fault handler

大致就是改写 `kernel/trap.c` 里的 `usertrap`. 我们在 `cow lab` 中可以改写过一次关于 `page fault` 的 handler 了。 这里和之前的类似。

首先需要注意一点: 当我们引发 `page fault`, 进入 `usertrap` 函数的时候， 我们还处于 __用户态__. 

```c
//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8)
  {
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
    
  } 
  //...
}
```

比如上方这段处理系统调用的 `usertrap`, 我们是在用户态发生 `trap (syscall / interrupt / exception)`, 然后 `usertrap` 获取必要的信息， 再进入内核态进程执行系统调用。

所以我们 `usertrap` 里面可以直接用 `myproc()` 函数拿到发生 `page fault` 的进程的 `process control block`.

我一开始实现成了这样:

```c
  else if(r_scause() == 13 || r_scause() == 15)
  {
    uint64 va = r_stval();
    // Notice that now we are in user mode
    // myproc() will return the process causing

    int have_found = 0;
    struct vma * pointer_to_vma;
    int i;
    for(i = 0; i < MAX_VMA; i++)
    {
      // Search every vma inside this process to find the vma which this page 
      // belongs to
      pointer_to_vma = &(p -> vma[i]);
      if(pointer_to_vma -> used == 0)
      {
        continue;
      }
      if(va >= pointer_to_vma -> starting_addr && 
         va < pointer_to_vma -> starting_addr + pointer_to_vma -> length)
      {
        // Check if va is inside the range of some vma
        // (We can view a vma mapping to a "memory block")
        have_found = 1;
        break;
      }
    }

    if(have_found == 0)
    {
      panic("Page fault but cannot find vma corresponding to it");
    }

    struct inode * ip = pointer_to_vma -> vma_file -> ip;
    // Get the inode pointer of this file because I need to read the data
    // of the first 4KB

    va = PGROUNDDOWN(va);
    uint64 pa;
    if((pa = (uint64)kalloc()) == 0)
    {
      panic("No available resouces to alloc");
    }
    // alloc the physical space
    // Now I haven't create any mapping in the page table

    memset((void * )pa, 0, PGSIZE);
    // initialization

    begin_op();
    // Remember to enclose every operation with inode
    // inside begin_op and end_op
    ilock(ip);

    uint64 read_file_offset = pointer_to_vma -> offset + (va - pointer_to_vma -> starting_addr);
    // I should read the first 4KB w.r.t va
    // So the starting point would be (va - addr_file) + offset

    if(readi(ip, 0, (uint64)pa, read_file_offset, PGSIZE) < 0)
    {
      // Read the data into pa
      iunlockput(ip);
      end_op();
      panic("can not read from the file");
    }

    iunlockput(ip);
    end_op();

    // Now set the PTE flags according to the 
    // vma flags
    // Notice that we manage the permission
    // by use of PTE flags (page wide permission)
    int flags = 0;
    if(pointer_to_vma -> prot & PROT_READ)
    {
      flags |= PTE_R;
    }
    if(pointer_to_vma -> prot & PROT_WRITE)
    {
      flags |= PTE_W;
    }
    if(pointer_to_vma -> prot & PROT_EXEC)
    {
      flags |= PTE_X;
    }
    flags |= PTE_U;
    // Let the user access this page

    // Create page table mapping
    if(mappages(p -> pagetable, va, PGSIZE, (uint64)pa, flags) < 0)
    {
      kfree((void * )pa);
      panic("Mapping failed");
    }
  }
  ```

  看起来没什么问题， 但是运行 `mmaptest` 会出现一个奇怪的 `panic inode`：

  ```
  $ mmaptest
mmap_test starting
test mmap f
panic: ilock
backtrace:
0x0000000080006c8c
0x0000000080003716
0x0000000080002284
QEMU: Terminated
```

debug 后发现是 `inode.refcnt == 0` 引起的 panic. 所以， 我是什么时候引起了 `recnt` 变成了 0 呢？

原因出在这里:

```c
    uint64 read_file_offset = pointer_to_vma -> offset + (va - pointer_to_vma -> starting_addr);
    // I should read the first 4KB w.r.t va
    // So the starting point would be (va - addr_file) + offset

    if(readi(ip, 0, (uint64)pa, read_file_offset, PGSIZE) < 0)
    {
      // Read the data into pa
      iunlockput(ip);
      end_op();
      panic("can not read from the file");
    }

    // iunlockput(ip);
    // At here, DO NOT use iput to drop a refcnt!!!
    // Because this inode is using along the mmap block
    // Then I should hold the refcnt along the mmap block
    iunlock(ip);
    end_op();
```

我这里不能 `iput`!!! 不然就释放了那个 `refcnt` 了。 这里只能 `iunlock` 释放锁， 不要调用 `iunlockput`.

修改完之后再运行， 可以发现过了许多测试。 

```c
$ mmaptest
mmap_test starting
test mmap f
test mmap f: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
mmaptest: mmap_test failed: file does not contain modifications, pid=3
panic: uvmunmap: not mapped
backtrace:
0x0000000080006c8c
0x00000000800009ce
0x0000000080000c56
0x00000000800012a2
0x00000000800012ea
```

接下来来实现 `munmap`.

## munmap

我最开始写成了这样:

```c
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
    return -1;
  }

  addr = PGROUNDDOWN(addr);
  length = PGROUNDUP(length);
  // ROUND addr and length

  if(pointer_to_vma -> flags & MAP_SHARED)
  {
    if(filewrite(pointer_to_vma -> vma_file, unmap_addr, PGSIZE) < 0)
    {
      return -1;
    }
  }

  uvmunmap(my_proc -> pagetable, unmap_addr, 1, 1);
  // unmap one page at a time


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
```

这样无法通过 `test not-mapped unmap`

```
panic: uvmunmap: not mapped
backtrace:
0x0000000080006e6c
0x00000000800009ce
0x0000000080000c56
0x00000000800012a2
0x00000000800012ea
0x0000000080001ca8
```

为什么会这样？ 这是因为这次 `unmap` 的那些 `page` 根本没有被访问过， 也就没有被分配。 那我 `uvmunmap` 它们， 或者读取它们的数据写入文件， 就自然会崩溃了。

所以， 我需要按 `page` 的颗粒度进行 `uvmunmap`: 对于每个 `page`, 我检查页表项的 `PTE_V` 来判断它是否被映射 (这个可以通过 `walkaddr` 的返回值实现)， 如果被映射， 我先检查是否要写回文件 (即是不是 `MAP_SHARED` )， 然后调用 `uvmunmap`, 否则什么都不做。 

所以我实现成了这样 (我写了很详细的注释):

```c
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
      break;
    }
  }

  if(has_found == 0)
  {
    return -1;
  }

  addr = PGROUNDDOWN(addr);
  length = PGROUNDUP(length);
  // ROUND addr and length

  for(int unmap_addr = addr; unmap_addr < addr + length; unmap_addr += PGSIZE)
  {
    if(walkaddr(my_proc -> pagetable, unmap_addr))
    {
      // walkaddr will return the physical address corresponding to 
      // vitual address "unmap_addr"
      // if it's 0, then it's unmapped
      // And I should NOT write to file or unmap the mapping if it's 0

      // Check if I need to write back to the file
      // Notice !! This must happen before doing the "uvmunmap"
      if(pointer_to_vma -> flags & MAP_SHARED)
      {
        if(filewrite(pointer_to_vma -> vma_file, unmap_addr, PGSIZE) < 0)
        {
          return -1;
        }
      }

      uvmunmap(my_proc -> pagetable, unmap_addr, 1, 1);
      // unmap one page at a time
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

```

现在终于过了 `munmap` 的测试:

```
$ mmaptest
mmap_test starting
test mmap f
test mmap f: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
test not-mapped unmap: OK
test mmap two files
test mmap two files: OK
mmap_test: ALL OK
fork_test starting
panic: uvmcopy: page not present
backtrace:
0x0000000080006e2c
0x0000000080000cf2
```

## modify exit

接下来修改以下 `exit` 系统调用， 退出之前把之前 `mmap` 的内存清理掉 (这样物理内存可以重新空闲出来):

```c
// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // unmap the previous mmap fields
  // Just as if munmap has been called
  struct vma * pointer_to_vma;
  for(int i = 0; i < MAX_VMA; i++)
  {
    pointer_to_vma = &(p -> vma[i]);
    if(pointer_to_vma -> used == 0)
    {
      continue;
    }
    uint64 addr = pointer_to_vma -> starting_addr;
    int length = pointer_to_vma -> length;

    addr = PGROUNDDOWN(addr);
    length = PGROUNDUP(length);
    // ROUND addr and length

    for(int unmap_addr = addr; unmap_addr < addr + length; unmap_addr += PGSIZE)
    {
      if(walkaddr(p -> pagetable, unmap_addr))
      {
        // walkaddr will return the physical address corresponding to 
        // vitual address "unmap_addr"
        // if it's 0, then it's unmapped
        // And I should NOT write to file or unmap the mapping if it's 0

        // Check if I need to write back to the file
        // Notice !! This must happen before doing the "uvmunmap"
        if(pointer_to_vma -> flags & MAP_SHARED)
        {
          if(filewrite(pointer_to_vma -> vma_file, unmap_addr, PGSIZE) < 0)
          {
            return;
          }
        }
        uvmunmap(p -> pagetable, unmap_addr, 1, 1);
        // unmap one page at a time
        fileclose(pointer_to_vma -> vma_file);
        pointer_to_vma -> used = 0;
      }
    }
  }

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++)
  {
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
  //...
}
```


## modify fork

直接在 `fork` 里面添加复制 `vma` 数组的部分即可:

```c
int
fork(void)
{
  //..
  struct vma * pointer_to_vma_p;
  struct vma * pointer_to_vma_np;
  // Just Copy the vma array
  for(int i = 0; i < MAX_VMA; i++)
  {
    pointer_to_vma_np = &(np -> vma[i]);
    pointer_to_vma_p = &(p -> vma[i]);
    pointer_to_vma_np -> used = 0;
    if(pointer_to_vma_p -> used)
    {
      pointer_to_vma_np -> used = pointer_to_vma_p -> used;
      pointer_to_vma_np -> starting_addr = pointer_to_vma_p -> starting_addr;
      pointer_to_vma_np -> length = pointer_to_vma_p -> length;
      pointer_to_vma_np -> prot = pointer_to_vma_p -> prot;
      pointer_to_vma_np -> flags = pointer_to_vma_p -> flags;
      strncpy(pointer_to_vma_np -> filename, pointer_to_vma_p -> filename, MAXPATH);
      pointer_to_vma_np -> fd = pointer_to_vma_p -> fd;
      pointer_to_vma_np -> offset = pointer_to_vma_p -> offset;
      pointer_to_vma_np -> vma_file = pointer_to_vma_p -> vma_file;
      filedup(pointer_to_vma_p -> vma_file);
    }
  }
  //..
}
```

## DEBUG

首先是发现了这里的 bug: `munmap` 的时候， 写回文件之前除了要检查 `MAP_SHARED`, 还要检查 `writable / PROT_WRITE`:

```c
 if((pointer_to_vma -> flags & MAP_SHARED)
       && (pointer_to_vma -> prot & PROT_WRITE) 
       && (pointer_to_vma -> vma_file -> writable))
      // Notice!! I also have to ensure that the file is writable to me
  {
    if(filewrite(pointer_to_vma -> vma_file, unmap_addr, PGSIZE) < 0)
    {
      return -1;
    }
  }
```

然后是 `exit` 也写出了 bug： 因为之后会统一关闭所有文件， 所以我不要对 `mmap` 对应的那些文件再 `fileclose` 一遍!

```c
  uvmunmap(p -> pagetable, unmap_addr, 1, 1);
  // unmap one page at a time

  //fileclose(pointer_to_vma -> vma_file);
  // DO NOT close the file here! Because we have all files closed below
  // If close the file here, It will lead to "panic : fileclose"

  pointer_to_vma -> used = 0;
```

# 通过基础测试！

最终终于通过了所有测试！！！

```
$ mmaptest
mmap_test starting
test mmap f
test mmap f: OK
test mmap private
test mmap private: OK
test mmap read-only
test mmap read-only: OK
test mmap read/write
test mmap read/write: OK
test mmap dirty
test mmap dirty: OK
test not-mapped unmap
test not-mapped unmap: OK
test mmap two files
test mmap two files: OK
mmap_test: ALL OK
fork_test starting
fork_test OK
mmaptest: all tests succeeded
```

# 总结梳理

最后我来梳理一下， 实现 `mmap` 需要哪些步骤?

+ 首先， 每个进程要设计一个数据结构来存 `mmap` 对应信息。

因此我设计了 `vma` 结构体， 每个进程存 `vma` 数组。

+ 然后， `mmap` 系统调用要正确配置好一个 `vma`.

这里如何找到一块合适的虚拟地址空间是一个难题。 也有很多优化点。 我这里实现的是朴素思路：直接从进程利用的最大空间向后延伸。

+ 由于是 `lazy allocation`, 所以要写好 `page fault handler`.

+ 对于 `munmap` 系统调用， 逐页分析是否被分配， 释放被分配的物理页。 

这一步很关键， 如果直接写整块的 `uvmunmap` 肯定是会报错的， 因为这块内存可能只有一部分被访问过从而被分配了页表对应的物理内存。 我们只能释放那些被分配过的内存。另外， 对于需要写回文件的页记得写回。

+ 修改好 `exit` 系统调用， 进程退出时也要像 `munmap` 一样释放 `mmap` 的内存并在必要时写回文件。

+ 最后修改好 `fork` 系统调用， 复制时也将 `vma` 进行复制。

这样就可以实现一个 `mmap` 了。 

# Improvement

上述过程实现了一个简易的 `mmap` 并通过了基础测试. 现在我们来考虑对它进行优化。 任务书上是这么写的:

```
Currently your implementation allocates a new physical page for each page read from the mmap-ed file, even though the data has been read in kernel memory in the buffer cache. Modify your implementation to use that physical memory instead of allocating a new page. This requires tha file blocks be the same size as pages (set BSIZE to PGSIZE). You will need to pin mmap-ed blocks into the buffer cache, reference counts should also be considered.
```

那就来按照这个思路优化它！ 如果对应 `page` 已经在 `buffer cache` 中了， 就直接从 `buffer cache` 中读取。

那我首先把 `BSIZE` 改一下:

```c
#define BSIZE PGSIZE  // block size
```

`buf` 的实现是这样的:

```c
struct buf 
{
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *next;
  struct buf *prev;
  uchar data[BSIZE];
  int ticks;
};
```

所以我们需要用的是 `data` 区域建立映射。

这里我的思路是， 既然拿到了这个 `buf` 的虚拟地址， 那就直接找到它的物理地址建立映射。 可它是在内核空间中的一块内存， 它对应的物理地址如何找到？ 下面我们需要深入理解 `xv6` 是如何管理内核地址的。

## 深入理解 trap 与内核线程

这里我们需要更深入地理解 `xv6` 中的 `trap` 与内核线程。 首先我们已经在 `trap lab` 中得知， 我们用户进程的地址空间是这样的：

<img src="https://notes.sjtu.edu.cn/uploads/upload_e9d4701b6d664098fa4d26cbfc6cbce4.png" width="500">

我们每个用户空间都会映射到一个独占的 `trapframe` 页， 这个页会在该进程陷入内核态时保存用户态的信息 (比如寄存器， program counter).

`trampoline` 是一段特殊的代码， 即 `kernel/trampoline.S`. 它只有一份， 被所有用户进程共享映射。 当用户进程需要陷入内核态时，会调用这里的代码进行一些配置。

另外注意，编译器编译时使用的地址都是 __offset（偏置）__. 真正执行这份程序时， 该进程的变量只会存在于该进程的空间中 (即同一份代码， 虚拟内存地址总是相同的， 但实际是物理内存地址可能不同). 把虚拟地址转换为物理地址是 `MMU (Memory Management Unit)` 为我们做的事情。 它会访问特定寄存器 `satp` 找到页表地址， 从而进行地址翻译。

我们现在考虑一个用户进程需要陷入内核态， 它调用 `trampoline.S`， 把用户进程信息保存在 `trapframe` 里面， 然后跳转到 `usertrap` 函数。 请注意， __进程切换__ 只是一个操作系统虚拟化出的概念， 这里相当于我们用户态进程进行了少许配置就开始完成内核工作， "变成" 了内核线程， 完成内核工作之后又会 "变回来"。 而且上述切换过程并没有修改 "每个 CPU 上对应线程 ID" 这个数据结构， 即

```c
// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}
```

`mycpu` 和 `myproc` 仍然会返回陷入内核态的用户进程的 ID. 所以， 我们实际上没有给 `内核进程` 设计 `process control block`. 我们这里是内核线程和用户进程的一对一模型。这种设计也有很多好处， 因为在陷入内核态之后我们往往还需要拿到之前对应的用户进程的 `process control block`. 

请注意， 为什么我说上述一直描述的是 "用户进程" 而不是 "用户线程"？ 因为 `xv6` 每个进程只有一份 `trapframe`， 当前版本的 `xv6` 并不支持同一个进程的多个进程同时陷入内核态。 如果两个线程同时陷入内核态， 它们会把线程寄存器存入同一张 `trapframe page` 上， 引起覆盖。 所以， 如果要支持多线程陷入内核态， 我们需要修改 `trapframe` 的存储结构 (比如改成每个线程对应独立的 `trapframe`).

现在还剩下一个问题: 内核的地址是怎么管理的？ 我们的 `xv6` 很简单， 它只有一个内核进程， 所有的内核线程是共享空间的， 而且这个空间大部分是 __直接映射__ 的， 只有 `trapframe` 和每个内核线程自己的栈空间不是直接映射的。下图是 `xv6` 的内核虚拟地址与物理地址的映射图。

<img src="https://notes.sjtu.edu.cn/uploads/upload_dd4edcb78fc53333a0e4fe0d0ca4c68d.png" width="500">


那既然是直接映射， 我们还需要为内核线程设置页表吗？ 答案是需要。 一方面， 并不是所有空间都是直接映射的。 另一方面， 因为 `MMU` 已经成为了硬件设计在体系结构中， 会自动将虚拟地址转换为物理地址。 如果没有明显收益的话，我们尽可能不添加额外逻辑绕开 `MMU`. 那解决方法也很简单: 我们创建一个直接映射的页表不就好了！ 然后我们把它存入 `trapframe -> kernel_satp` 中。

`xv6` 是这样做的:

首先， 我们在操作系统启动的时候 (即 main 函数)， 会调用 `kernel/vm.c` 中的这些函数配置好 `kernel pagetable`:

```c
// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0)
  {
    //...
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    //...
    userinit();      // first user process
  } 
  else 
  {
    //...
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    //...
  }

  scheduler();        
}
```

我们来看这几个函数的实现:

### kvminit

```c
// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}
```

`kvminit` 会调用 `kvmmake` 创建内核页表。

### kvmmake

```c
// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}
```

`kvmmake` 会直接映射大部分空间。 然后调用 `proc_mapstacks` 来映射栈空间。

### proc_mapstacks

```c
// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) 
  {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}
```

请注意， 我们直接分配了 `NPROC` 个内核线程的栈空间并将其映射到了内核页表中！ 因为我们是 __每一个用户进程对应一个内核线程__， 因此这样分配是正确的。 或许我们可以采取 `lazy allocation` 的策略对这里进行优化， 不过我们先暂时抓住主题思想， 即每个内核线程有自己独立的栈空间， 它是非直接映射的。

### kvminithart

```c
// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}
```

这里会调用 `w_satp` 配置内核页表至 `satp` 寄存器，我们来看这个函数.

### w_satp

```c
// supervisor address translation and protection;
// holds the address of the page table.
static inline void 
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r" (x));
}
```

这里 `w_satp` 会调用 `compare and swap` 指令写入 `satp` 寄存器， 即配置 `kernel pagetable` 进入寄存器。

请注意， 只有 `cpuid = 1` 的 CPU 会调用 `kvminit` 函数创建内核页表， 但是所有 `CPU` 都会调用 `kvminithart` 把页表寄存器配置为内核页表。 所以执行完之后所有页表寄存器都是内核页表了。

而当我们创建了用户进程并切换之后， 就会把 `satp` 换成用户进程的页表了， 而把 `kernel pagetable` 存入用户进程的 `proc -> trapframe -> kernel_satp` 了。

## 理解 readi 内部细节

理解了内核地址管理之后， 我们来看我们如何改进 `mmap` 的内存分配。

我们之前对于 `page fault handler` 的实现是这样的:

```c
    //...
    begin_op();
    // Remember to enclose every operation with inode
    // inside begin_op and end_op
    ilock(ip);

    uint64 read_file_offset = pointer_to_vma -> offset + (va - pointer_to_vma -> starting_addr);
    // I should read the first 4KB w.r.t va
    // So the starting point would be (va - addr_file) + offset

    if(readi(ip, 0, (uint64)pa, read_file_offset, PGSIZE) < 0)
    {
      // Read the data into pa
      iunlockput(ip);
      end_op();
      panic("can not read from the file");
    }

    // iunlockput(ip);
    // At here, DO NOT use iput to drop a refcnt!!!
    // Because this inode is using along the mmap block
    // Then I should hold the refcnt along the mmap block
    iunlock(ip);
    end_op();

    // Now set the PTE flags according to the 
    // vma flags
    // Notice that we manage the permission
    // by use of PTE flags (page wide permission)
    int flags = 0;
    if(pointer_to_vma -> prot & PROT_READ)
    {
      flags |= PTE_R;
    }
    if(pointer_to_vma -> prot & PROT_WRITE)
    {
      flags |= PTE_W;
    }
    if(pointer_to_vma -> prot & PROT_EXEC)
    {
      flags |= PTE_X;
    }
    flags |= PTE_U;
    // Let the user access this page

    // Create page table mapping
    if(mappages(p -> pagetable, va, PGSIZE, (uint64)pa, flags) < 0)
    {
      kfree((void * )pa);
      panic("Mapping failed");
    }
    //...
```

这里我们来看一下 `readi` 的实现细节, 我为它写了详细注释:

```c
// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  // tot is the number of types that have been written
  struct buf * bp;

  if(off > ip -> size || off + n < off)
  {
    // If the offset exceed the length of the file, report error
    // If offset plus n will overflow, report error
    return 0;
  }

  if(off + n > ip -> size)
  {
    // If offset plus n will exceed the length of the file
    // modify n, to end at the end of the file
    n = ip -> size - off;
  }

  for(tot = 0; tot < n; tot += m, off += m, dst += m)
  {
    uint addr = bmap(ip, off / BSIZE);
    // Use bmap to get the address of the block

    if(addr == 0)
    {
      // bmap failed
      break;
    }
    bp = bread(ip -> dev, addr);
    // use bread to read the block into a buffer in the buffer cache
    // and return the buffer

    m = min(n - tot, BSIZE - off % BSIZE);
    // update "m"

    // either_copyout will copy the data in the buffer cache 
    // into a kernel space or user space
    if(either_copyout(user_dst, dst, bp -> data + (off % BSIZE), m) == -1) 
    {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}
```

其中的 `either_copyout` 函数是这样实现的:

```c
// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc * p = myproc();
  if(user_dst)
  {
    // If the destination is a user space
    // Then dst is a virtual address
    // Use the pagetable and the copyout function
    return copyout(p -> pagetable, dst, src, len);
  }
  else 
  {
    // If the destination is a kernel space
    // Then dst is directed mapped 
    // (Only stack space and trampoline are not directed mapped)
    memmove((char *)dst, src, len);
    return 0;
  }
}
```

所以问题出在 `either_copyout`上: 我现在已经把文件的某些 `block` 读到内核空间的 `buffer cache` 里面了， 之后却又要新分配一个物理内存，然后把 `buffer cache` 里的东西复制过去。 这里产生了浪费！

那我们修改这个函数好了。 