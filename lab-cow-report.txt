### 任务简介

这个lab就是实现copy-on-write fork。 
用lazy-allocation的思想： 当调用fork的时候， 先让子进程和父进程真的共用同一块内存， 只有一方需要写入的时候才真正复制一份。

```
The goal of copy-on-write (COW) fork() is to defer allocating and copying physical memory pages for the child until the copies are actually needed, if ever.

COW fork() creates just a pagetable for the child, with PTEs for user memory pointing to the parent's physical pages. COW fork() marks all the user PTEs in both parent and child as not writable. When either process tries to write one of these COW pages, the CPU will force a page fault. The kernel page-fault handler detects this case, allocates a page of physical memory for the faulting process, copies the original page into the new page, and modifies the relevant PTE in the faulting process to refer to the new page, this time with the PTE marked writeable. When the page fault handler returns, the user process will be able to write its copy of the page.

COW fork() makes freeing of the physical pages that implement user memory a little trickier. A given physical page may be referred to by multiple processes' page tables, and should be freed only when the last reference disappears.
```

### 主体思路

首先， 我要维护每个物理页的refcnt. 我用一个数组来维护， 然后每个成员要上锁（保证修改refcnt是原子操作）
我写好refcnt的原子操作， 提供好接口。

```c
struct refcnt

{

  struct spinlock lock;

  int count;

}; //refcnt is a type representing a refcnt and corresponding lock

//for a physical page

  

struct refcnt count_array[(PHYSTOP - KERNBASE) / PGSIZE + 1];

//This is a count array used for cow-fork

//to record the number of references to each page

//Notice: physical memory that we can access start from

//KERNBASE and end at PHYSTOP

  

void count_incre(uint64 pa)

//atomic "refcnt++""

{

  int index = (pa - KERNBASE) / PGSIZE;

  acquire(&count_array[index].lock);

  count_array[index].count++;

  release(&count_array[index].lock);

}

  

void count_init(uint64 pa, int num)

//atomic "refcnt = num"

{

  int index = (pa - KERNBASE) / PGSIZE;

  acquire(&count_array[index].lock);

  count_array[index].count = num;

  release(&count_array[index].lock);

}

  

void count_decre(uint64 pa)

//atomic "refcnt--"

{

  int index = (pa - KERNBASE) / PGSIZE;

  acquire(&count_array[index].lock);

  count_array[index].count--;

  release(&count_array[index].lock);

}

  

int count_check(uint64 pa, int expected)

//atomic "return refcnt == expected"

{

  int index = (pa - KERNBASE) / PGSIZE;

  acquire(&count_array[index].lock);

  int cnt = count_array[index].count;

  release(&count_array[index].lock);

  return cnt == expected;

}
```


既然我新加入了refcnt， 那我就要维护好它。 首先, kinit里面要初始化锁。 然后kinit会调用freerange清空空间， 我要在freerange结束后保证那块空间被kfree清理掉了：

```c
void

freerange(void *pa_start, void *pa_end)

{

  char *p;

  p = (char*)PGROUNDUP((uint64)pa_start);

  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){

    count_init((uint64)p, 1);

    //set the refcnt to 1

    kfree(p);

    //decre the refcnt to 0 and free it

  }

}
```

kfree里面有count_decre:

```c
void

kfree(void *pa)

{

  struct run *r;

  

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)

    panic("kfree");

  

  count_decre((uint64)pa);

  if(!count_check((uint64)pa, 0))

    return;

  

  // Fill with junk to catch dangling refs.

  memset(pa, 1, PGSIZE);
  ...
 }
```

对于kalloc，我需要初始化新alloc的Page的refcnt是1.

然后就开始写kernel/vm.c.

我先在riscv.h的里面为page table entry定义一个自定义的BIT：PTE_COW。 它被set了就表示这个page table entry是cow-fork出来指向已经存在的物理页的：

```c
#define PTE_COW (1L << 8) //reserved for software
```

接着我把uvmcopy从真正复制物理内存的版本修改成cow fork版本：

```c
//COW fork version

int

uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)

{

  pte_t *pte;

  uint64 pa, i;

  

  for(i = 0; i < sz; i += PGSIZE)

  {

    pte = walk(old, i, 0);

    //find the pte with the old pagetable

    if(!pte)

    {

      panic("uvmcopy: pte should exist");

    }

    if((*pte & PTE_V) == 0)

    {

      //This means it's not valid

      panic("uvmcopy: page not present");

    }

    pa = PTE2PA(*pte);

    //find the physical address

    if(*pte & PTE_W)

    {

      //if it's writable, then just copy it

      //and set it to none writable and cow

      *pte = (*pte & (~PTE_W)) | PTE_COW;

    }

  

    //Then map it to the new pagetable

    if(mappages(new, i, PGSIZE, (uint64)pa, PTE_FLAGS(*pte)))

    {

      uvmunmap(new, 0, i / PGSIZE, 1);

      return -1;

    }

  

    count_incre(pa);

    //remember to increment the reference count

  }

  return 0;

}
```

值得注意的是， 这样修改完之后， 每次调用uvmcopy都不可能复制真正的物理内存， 只可能lazy copy。 只有真正写入的时候进入User trap， cow_fault_handler才会来进行真正的复制。


修改下usertrap:

```c kernel/trap.c
void

usertrap(void)

{
....
    syscall();

  }

  else if((which_dev = devintr()) != 0)

  {

    // ok

  }

  else if(r_scause() == 15) // cow fault happen

  {

    if(!cow_fault_handler(p -> pagetable, r_stval()))

    { //try to fix it: it will try to alloc a new physical page

      //and do all the stuffs like deleting and creating mappings

      //If it still failed, kill this process

      setkilled(p);

    }

  }

....
}
```


我的cow_fault_handler是这么实现的：

```c kernel/vm.c
//return 0 if fail

//otherwise, return the physical address of the right address

//after handling cow fault

uint64 cow_fault_handler(pagetable_t pagetable, uint64 va)

{

  

  uint64 pa;

  if(va > MAXVA)

  {

    return 0;

  }

  pte_t *pte = walk(pagetable, va, 0);

  if(pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U))

  //if I cannot find the pte corresponding to the va

  //or the pte is not valid

  //or the pte is not accessible to user

  //then return 0

  {

    return 0;

  }

  
  

  pa = PTE2PA(*pte);

  

  //If it's writable, write to it directly

  if(*pte & PTE_W)        

  {  

    return pa;

  }

  

  //if it's not writable and not cow

  //return 0

  if(!(*pte & PTE_COW))

  {

    return 0;

  }

  

  //then it's not writable and it's cow

  //deal with cow fault right now

  int flags = PTE_FLAGS(*pte);

  //copy the flags

  

  uint64 new_pa = (uint64)kalloc();

  //alloc a new space

  

  if(new_pa == 0)

  {

    return 0;

  }

  memmove((void * )new_pa, (const void * )pa, PGSIZE);

  //move the contents from the old physical address to the new physical address

  

  uvmunmap(pagetable, PGROUNDDOWN(va), 1, 1);

  //unmap the old va with the old pte

  

  flags = (flags | PTE_W) & (~PTE_COW);

  // enable writing and it's not cow now

  

  if(mappages(pagetable, PGROUNDDOWN(va), PGSIZE, new_pa, flags))

  //map the va with the new_pa

  {

    //mapping failed

    kfree((void * ) new_pa);

    return 0;

  }

  return new_pa;

}
```


最后， 由于copyout这个函数是把内核态的东西复制去用户态，但是内核态里面我就没有usertrap了， 那如果用户态的虚拟地址对应的是cow page， 那我就不能直接写， 我要调用cow_fault_handler。 注意cow_fault_handler这里是通用的。 如果是直接可以写的Page， 它就直接返回对应的physical page； 如果是cow page, 它就创建新的物理页来返回。

```c
// Copy from kernel to user.

// Copy len bytes from src to virtual address dstva in a given page table.

// Return 0 on success, -1 on error.

int

copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)

{

  uint64 n, va0, pa0;

  

  while(len > 0)

  {

    va0 = PGROUNDDOWN(dstva);

    pa0 = cow_fault_handler(pagetable, va0);

    // use cow_fault_handler to output a writable physical address

    // notice : maybe the pa0 corresponding va0 is not cow

    // but that doesn't matter, in this case it will return the original physical address

    // cow_fault_handler will always find a writable one

  

    if(pa0 == 0)

    {

      return -1;

    }

    n = PGSIZE - (dstva - va0);

    if(n > len)

      n = len;

    memmove((void *)(pa0 + (dstva - va0)), src, n);

  

    len -= n;

    src += n;

    dstva = va0 + PGSIZE;

  }

  return 0;

}
```

### 细节梳理

最后梳理一下， cow-fork到底发生了什么：

1.用户调用fork接口函数， 触发系统调用， 进入内核态， 调用对应函数指针， sys_fork调用fork函数

```c kernel/sysproc.c
uint64

sys_fork(void)

{

  return fork();

}
```

2.fork函数调用uvmcopy来复制物理内存

```c kernel/proc.c
// Create a new process, copying the parent.

// Sets up child kernel stack to return as if from fork() system call.

int

fork(void)

{

  int i, pid;

  struct proc *np;

  struct proc *p = myproc();

  

  // Allocate process.

  if((np = allocproc()) == 0){

    return -1;

  }

  

  // Copy user memory from parent to child.

  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){

    freeproc(np);

    release(&np->lock);

    return -1;

  }

  np->sz = p->sz;

  

  // copy saved user registers.

  *(np->trapframe) = *(p->trapframe);
  ....
```

然后uvmcopy用lazy copy(cow)的思想， 先只复制pagetable的映射， 而不复制真正的物理内存。

当父进程或者子进程一方要写入这个page的时候， 会发现PTE_COW bit权限不允许， 因此引发usertrap, 硬件设置trap原因 r_scause 为15, r_stavl 为 Page。 然后调用我定制的cow_fault_handler 新alloc一个物理页， 维护好映射， 把想写入的东西写入新创建的页。

### 遇到的困难

最开始写完booting出现死循环了：

```c
balloc: first 964 blocks have been allocated
balloc: write bitmap block at sector 45
qemu-system-riscv64 -machine virt -bios none -kernel kernel/kernel -m 128M -smp 3 -nographic -global virtio-mmio.force-legacy=false -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

xv6 kernel is booting
```

最后发现是一个地方把

```c
count_array[(pa - KERNBASE) / PGSIZE]
```

里面的除 PGSIZE 忘写了。。。

改好了之后通过大部分测试。

```shell
$ cowtest
simple: ok
simple: ok
three: ok
three: ok
three: ok
file: pipe() failed
$ 
```

但是usertests从copyout就开始挂了

```c
$ make qemu-gdb
(19.4s) 
== Test   simple == 
  simple: OK 
== Test   three == 
  three: OK 
== Test   file == 
  file: OK 
== Test usertests == 
$ make qemu-gdb
(7.2s) 
    ...
         test copyout: usertrap(): unexpected scause 0x0000000000000002 pid=6
                     sepc=0x00000000000006f8 stval=0x0000000000000000
         FAILED
         SOME TESTS FAILED
         $ qemu-system-riscv64: terminating on signal 15 from pid 2183946 (make)
    MISSING '^ALL TESTS PASSED$'
    QEMU output saved to xv6.out.usertests
== Test   usertests: copyin == 
  usertests: copyin: FAIL 
    Parent failed: test_usertests
== Test   usertests: copyout == 
  usertests: copyout: FAIL 
    Parent failed: test_usertests
== Test   usertests: all tests == 
  usertests: all tests: FAIL 
    Parent failed: test_usertests
== Test lab-cow-report.txt == 
lab-cow-report.txt: FAIL 
    Cannot read lab-cow-report.txt
Score: 80/114
make: *** [Makefile:339: grade] Error 1
```

然后我开始debug。 后来我怎么都过不了usertests, 我就新开了一个branch半重构来写。 我找到了以下这些BUG： （很小的bug就不写上来了， 像某个 return 忘写， bit写错， 条件写反那种。 下面只列举大的BUG）

### BUG1:

最开始的uvmcopy里面， 我是这么写的， 也就是无论如何都取消 write bit ， 添加 cow bit.

```c


      *pte &= ~PTE_W;

      *pte |= PTE_COW;

```

但是这根本不对啊！有些地方本来就没有write权限，是只读区域。如果这么写岂不是把PTE_W bit反转了， 变成可写入的区域了？所以这样写肯定是错的。

最后改成了这样：

```c
  if(*pte & PTE_W)

    {

      //if it's writable, then just copy it

      //and set it to none writable and cow

      *pte = (*pte & (~PTE_W)) | PTE_COW;

    }
```


### BUG2:

copyout部分， 最开始写成了这样：

```c

    if(!(*pte & PTE_W))

    {

      pa0 = cow_fault_handler(pagetable, va0);

      if(pa0 == 0)

      {

        return -1;

      }
...
```

但是其实这跟BUG1错的地方是一样的。 如果用户态的这块区域是只读的， 那我根本就不能写入， 应该报错才行。 所以并不是用PTE_W来判断是不是cow page, 只能用PTE_COW。

后来我把cow_fault_handler通用化了， 写成了这样：

```c
int

copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)

{

  uint64 n, va0, pa0;

  

  while(len > 0)

  {

    va0 = PGROUNDDOWN(dstva);

    pa0 = cow_fault_handler(pagetable, va0);

    // use cow_fault_handler to output a writable physical address

    // notice : maybe the pa0 corresponding va0 is not cow

    // but that doesn't matter, in this case it will return the original physical address

    // cow_fault_handler will always find a writable one

    if(pa0 == 0)

    {

      return -1;

    }
    ...
  }
```


### BUG3:


cow_fault_handler 最开始写成了：

```c
pte_t *pte = walk(pagetable, va, 0);

  if(pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U) || !(*pte & PTE_COW))

  { return 0;

  }

  pa = PTE2PA(*pte);

  

  if(*pte & PTE_W)        

  {  

    return pa;

  }


```

但是这样逻辑上是不对的：如果它不是cow page, 但是是一个可写page， 那不应该直接返回对应的physical page吗？（我的cow_fault_handler是通用的）

改成这样就对了：

```c
pte_t *pte = walk(pagetable, va, 0);

  if(pte == 0 || !(*pte & PTE_V) || !(*pte & PTE_U))

  //if I cannot find the pte corresponding to the va

  //or the pte is not valid

  //or the pte is not accessible to user

  //then return 0

  {

    return 0;

  }

  
  

  pa = PTE2PA(*pte);

  

  //If it's writable, write to it directly

  if(*pte & PTE_W)        

  {  

    return pa;

  }

  

  //if it's not writable and not cow

  //return 0

  if(!(*pte & PTE_COW))

  {

    return 0;

  }
```

最后我顺利完成了测试：

```shell
xv6 kernel is booting

hart 1 starting
hart 2 starting
init: starting sh
$ cowtest
simple: ok
simple: ok
three: ok
three: ok
three: ok
file: ok
ALL COW TESTS PASSED
$ usertests
usertests starting
test copyin: OK
test copyout: OK
test copyinstr1: OK
test copyinstr2: OK
test copyinstr3: OK
test rwsbrk: OK
test truncate1: OK
test truncate2: OK
test truncate3: OK
test openiput: OK
test exitiput: OK
test iput: OK
test opentest: OK
test writetest: OK
test writebig: OK
test createtest: OK
test dirtest: OK
test exectest: OK
test pipe1: OK
test killstatus: OK
test preempt: kill... wait... OK
test exitwait: OK
test reparent: OK
test twochildren: OK
test forkfork: OK
test forkforkfork: OK
test reparent2: OK
test mem: OK
test sharedfd: OK
test fourfiles: OK
test createdelete: OK
test unlinkread: OK
test linktest: OK
test concreate: OK
test linkunlink: OK
test subdir: OK
test bigwrite: OK
test bigfile: OK
test fourteen: OK
test rmdot: OK
test dirfile: OK
test iref: OK
...
```

另外我发现有时候make grade会卡顿， 不知道是不是我本机的问题， 反正就卡住了测不下去。  但是我本机跑

```shell
make clean
make qemu
cowtest
usertests
```

是可以过的。

这样就顺利完成了这个lab。 这个lab对我来说还是挺难的， 花了好多天才做完。