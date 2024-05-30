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


## DEBUG

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