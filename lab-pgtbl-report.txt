### Speeding up system calls

这个任务的意思就是， 我每次要读进程Pid的时候还要system call切内核态，实在是太慢了。 我还不如把这个值写入用户态的内存空间里面（反正这个值也不会改了）。 这样getpid就不用切内核态了。

测试用的ugetpid已经被写好了：

```c /kernel/ulib.c
#ifdef LAB_PGTBL

int

ugetpid(void)

{

  struct usyscall *u = (struct usyscall *)USYSCALL;
//USYSCALL是一个虚拟地址， 用一个指针指向它
//为什么我不是用进程的p -> usyscall？
//因为那个是物理地址， 那个地址为了构建page table临时存储在struct proc里面的， 建立完page table了就不需要访问它了
  return u->pid;
//然后返回pid
}

#endif
```


kernel.memlayout.h 已经为我们定义好了 struct usyscall

```c
#ifdef LAB_PGTBL

#define USYSCALL (TRAPFRAME - PGSIZE)
//表示USYSCALL这个虚拟地址在TRAPFRAM正下方一个Page
  
struct usyscall {

  int pid;  // Process ID
//就存一个pid
};

#endif
```

那么就很简单了。 进程状态struct 里面加一个区域存usyscall的物理地址， 用于构建这部分page table。我在进程创建的时候， 给p -> usyscall 分配好物理内存地址， 在建立page table的时候建立好虚拟地址(USYSCALL)到物理地址(p -> usyscall)的映射。然后进程销毁的时候记得也要回收这块内存以及销毁page table中的映射。

具体而言， 我给proc加一个指向usyscall的指针临时存分配到的物理内存地址（因为后面建立page table和释放内存的时候还要用， 就必须把它跟trapframe一样把物理地址存在进程的struct proc里面）：

```c
// Per-process state

struct proc {

  struct spinlock lock;

  //stores the physical address of usyscall

  struct usyscall * usyscall;


  // p->lock must be held when using these:

  enum procstate state;        // Process state

  void *chan;                  // If non-zero, sleeping on chan
...
}

```

进程创建时分配物理内存：

```c
//newly added: alloc a physical page for usyscall

  //and assign the pid to this struct

  if((p -> usyscall = (struct usyscall *)kalloc()) == 0)

  {

    freeproc(p);

    release(&p -> lock);

    return 0;

  }
```

建立page table时建立关于usyscall的映射， 仿照TRAPFRAM的那个来写：

```c
 //map the usyscall page

  //read only for user process
									
   if(mappages(pagetable, USYSCALL, PGSIZE,

              (uint64)(p -> usyscall), PTE_R | PTE_U) < 0)

  {

    uvmunmap(pagetable, USYSCALL, 1, 0);

    uvmfree(pagetable, 0);

    return 0;

  }
```

销毁时释放物理内存：

```c
//Newly added : free the physical memory used for usyscall

  if(p -> usyscall)

  {

    kfree((void*)p -> usyscall);

  }

  p -> usyscall = 0;
```

销毁时一同销毁page table:

```c
void

proc_freepagetable(pagetable_t pagetable, uint64 sz)

{

  uvmunmap(pagetable, TRAMPOLINE, 1, 0);

  uvmunmap(pagetable, TRAPFRAME, 1, 0);

  uvmunmap(pagetable, USYSCALL, 1, 0);

  uvmfree(pagetable, sz);

}
```

#### 遇到的困难

写出了 usertrap:
```shell
$ pgtbltest
ugetpid_test starting
usertrap(): unexpected scause 0x000000000000000d pid=4
            sepc=0x0000000000000492 stval=0x0000003fffffd000
```

这个真的调不出来是哪错了， 后来是找到有人写的经验帖子才明白我是哪里写错了：

我在mappages的时候， 只给了PTE_R(只读) 权限， 没给 PTE_U 权限。 主要是我是仿照着上面Trapframe的格式写的。 而不给PTE_U权限的话这块区域根本不能被用户访问， 只能在supervisor mode下访问。所以我加上PTE_U就好了。

最后的mappages应该是这样：

```cpp
//map the usyscall page

  //read only for user process

   if(mappages(pagetable, USYSCALL, PGSIZE,

              (uint64)(p -> usyscall), PTE_R | PTE_U) < 0)

  {

    uvmunmap(pagetable, USYSCALL, 1, 0);

    uvmfree(pagetable, 0);

    return 0;

  }
```


### print a page table

这里就是在kernel/vm.c里面加一个vmprint， 用于输出pagetable. 大致就是仿照给出的freewalk函数来写。

### 遇到的困难

最开始把函数写成了这样：

```c
//helper function for vmprint

void printwalk(pagetable_t pagetable, int level)

{

  for(int i = 0; i < 512; i++)

  {

    pte_t pte = pagetable[i];

    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0)

    {

      // this PTE points to a lower-level page table.

      uint64 child = PTE2PA(pte);

      printwalk((pagetable_t)child, level + 1);

    }

    else if(pte & PTE_V)

    {

      panic("printwalk: leaf");

    }

  }

  for(int i = 0; i < level; i++)

  {

    printf(  "..");

  }

  printf("%p\n", pagetable);

}

  

//print out the page table

void vmprint(pagetable_t pagetable)

{
  printf("page table, %p\n", pagetable);
  printwalk(pagetable, 1);

}
```

然后测试未通过

```shell
pte printout: FAIL (1.3s) 
    ...
         hart 2 starting
         init: starting sh
         $ echo hi
         hi
         $ qemu-system-riscv64: terminating on signal 15 from pid 2101306 (make)
```

排查之后发现，这里有两个问题。 首先是没按照规定把vmprint加到exec里面， 然后代码逻辑也错了。 我上面这样写的话， 相当于DFS， 会倒过来输出。 但是我这里希望是按照这个格式输出：

```shell
page table 0x0000000087f6e000
 ..0: pte 0x0000000021fda801 pa 0x0000000087f6a000
 .. ..0: pte 0x0000000021fda401 pa 0x0000000087f69000
 .. .. ..0: pte 0x0000000021fdac1f pa 0x0000000087f6b000
 .. .. ..1: pte 0x0000000021fda00f pa 0x0000000087f68000
 .. .. ..2: pte 0x0000000021fd9c1f pa 0x0000000087f67000
 ..255: pte 0x0000000021fdb401 pa 0x0000000087f6d000
 .. ..511: pte 0x0000000021fdb001 pa 0x0000000087f6c000
 .. .. ..509: pte 0x0000000021fdd813 pa 0x0000000087f76000
 .. .. ..510: pte 0x0000000021fddc07 pa 0x0000000087f77000
 .. .. ..511: pte 0x0000000020001c0b pa 0x0000000080007000
```

之后改成这样终于对了：

```c
//helper function for vmprint

void printwalk(pagetable_t pagetable, int level)

{

  for(int i = 0; i < 512; i++)

  {

    pte_t pte = pagetable[i];

    if((pte & PTE_V))

    {

      // this PTE points to a lower-level page table.

      uint64 child = PTE2PA(pte);

      for(int i = 0; i < level; i++)

      {

        if(i == 0)

        {

          printf("..");

        }

        else

        {

        printf(" ..");

        }

      }

      printf("%d: pte %p pa %p\n", i, pte, child);

      if((pte & (PTE_R | PTE_W | PTE_X)) == 0)

      {

        printwalk((pagetable_t)child, level + 1);

      }

    }

    else if(pte & PTE_V)

    {

      panic("printwalk: leaf");

    }

  }

}

  

//print out the page table

void vmprint(pagetable_t pagetable)

{

  printf("page table %p\n", pagetable);

  printwalk(pagetable, 1);

}
```


###  Detecting which pages have been accessed

这个任务就是要写一个pgaccess系统调用， 用于检测一段连续的虚拟内存中， 哪些page是被访问过的。 page的访问状态由Page table entry的PTE_A表征。

这个系统调用的用法：

```c 
if (pgaccess(buf, 32, &abits) < 0)
//buf是虚拟内存起始位置， 32表示连续检查32个page, abits是给出一个地址用于存结果（结果是一个mask int, 第i位为1表示第i个page被访问过）
    err("pgaccess failed");

  buf[PGSIZE * 1] += 1;

  buf[PGSIZE * 2] += 1;

  buf[PGSIZE * 30] += 1;

  if (pgaccess(buf, 32, &abits) < 0)

    err("pgaccess failed");
```

根据 riscV 规则定义好 PTE_A bit:

```c
#define PTE_A (1L << 6) // accessed
```

然后就直接来写这个系统调用。 因为已经有贴心的walk函数可以用虚拟地址找到物理内存地址， 所以还是好写的：

```c
pte_t *

walk(pagetable_t pagetable, uint64 va, int alloc);
```

最开始写成了这样：

```c
uint64

sys_pgaccess(void)

{

  uint64 start_addr;

  argaddr(0, &start_addr);

  int num;

  argint(1, &num);

  uint64 address;

  argaddr(2, &address);

  unsigned int mask = 0x0;

  struct proc * my_proc = myproc();

  pagetable_t * pagetable = &(my_proc -> pagetable);

  pte_t * pte = walk(*pagetable, start_addr, 0);

  for(int i = 0; i < num; i++)

  {

    pte = walk(*pagetable, start_addr + i * PGSIZE, 0);
    if(pte == 0)

    {

      return -1;

    }

    mask |= ((*pte & PTE_A) << i);

  }

  copyout(*pagetable, address, (char *)&mask, sizeof(mask));

  return 0;

}
```

测试又没过。。。

```shell
$ pgtbltest
ugetpid_test starting
ugetpid_test: OK
pgaccess_test starting
pgtbltest: pgaccess_test failed: incorrect access bits set, pid=3
```

后来发现有两个问题
1.mask每次修改规则写错了
2.pgaccess 检查完一个page之后没有把PTE_A恢复。

改成这样就过了：

```c
uint64

sys_pgaccess(void)

{

	//解析函数参数
  uint64 start_addr;

  argaddr(0, &start_addr);

  int num;

  argint(1, &num);

  uint64 address;

  argaddr(2, &address);

  unsigned int mask = 0x0;

  struct proc * my_proc = myproc();

  pagetable_t * pagetable = &(my_proc -> pagetable);

  for(int i = 0; i < num; i++)

  {

    pte_t * pte = walk(*pagetable, start_addr + i * PGSIZE, 0);
	//注意， 虚拟内存连续但是物理内存不一定连续， 所以每次都必须用walk来找
    //use walk to find the page table entrys

    if(pte == 0)

    {

      return -1;

    }

    if(*pte & PTE_A)
	//说明PTE_A这个Bit被硬件set了， 它是我们需要标记的page
    {

      mask |= (1 << i);
		//标记它
      //This means this page is accessed

    }

    *(pte) &= ~PTE_A;
	//check完要清理这个Bit。 不要让它永远被set上了。
    //erase the PTE_A bit(I just check, this is not access)

  }

  copyout(*pagetable, address, (char *)&mask, sizeof(mask));
//用copyout从内核空间复制到用户空间
  return 0;

}
```