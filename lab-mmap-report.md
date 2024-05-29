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



