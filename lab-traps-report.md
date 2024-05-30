# 操作系统 trap lab 报告

我是用 markdown 写的报告，所以可能报告用 markdown 阅读器打开会更方便批阅一些。

-------------------------------------------------------------------------

trap的来源可能是 interrupt / expection / system call. 

### backtrace

这个 lab 给我们提供了一个有用的内嵌汇编函数：

```c
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

因为编译器 (gcc) 把当前执行函数对应的 frame pointer （存 frame 开头地址） 存在了 s0 寄存器， 所以我用这个函数就可以读到那个 frame 的地址。

然后我首先在 defs.h 里加 backtrace(void) 的原型， 在 kernel/sysproc.c 里面给 sys_sleep 加对于 backtrace 的调用（我发现这里已经贴心得有了 "#ifdef ... #endif"）

之后就是照着说明书的图来写。

我一开始写成了这样：

```cpp
void
backtrace(void)
{
  uint64 frame_pointer;
  uint64 return_address;
  struct proc *p = myproc();
  if (p == 0)
  {
    return;
    //myproc failed
  }
  frame_pointer = r_fp();//Get the frame pointer using inline assembly call r_fp()

  printf("backtrace:\n");
  while (frame_pointer != 0) 
  {
    return_address = *(uint64 *)(frame_pointer - 8);
    //Get the return address of the current stack frame
    //Due to the graph, it's 
    printf("%p\n", return_address);
    frame_pointer = *(uint64 *)(frame_pointer - 16);
    //Get he frame pointer of the previous stack frame
  }
}
```

乍一看好像没啥问题， 但我一运行测试的命令 betest, 发现：

```cpp
$ bttest
backtrace:
0x0000000080002272
0x0000000080002102
0x0000000080001df6
0x0000000000000012
scause 0x000000000000000d
sepc=0x0000000080005fdc stval=0x0000000000003fc8
panic: kerneltrap
```

然后顺便把我的电脑散热模块干烧起来了（不知道是不是巧合，但是确实风扇转得很快）, 命令行也死机了 ctrl + c 退不出来， 只能强制关掉。

后来才发现， 我这个循环条件写得很有问题：


```cpp
while (frame_pointer != 0) 
```

frame_pointer 在我运行的时候直接跑到了 0x0d 这么低的位置。 凭借我的想象， 这么低的位置对于一个进程来说可能是很重要的东西， （不过感觉这操作系统的安全性也不够居然没有及时报 exception？ ） 所以显然我的 循环条件写得有问题。 

我再去看了一遍 lab 要求才发现自己遗漏了这段话： 

```
Xv6 allocates one page for each stack in the xv6 kernel at PAGE-aligned address. You can compute the top and bottom address of the stack page by using PGROUNDDOWN(fp) and PGROUNDUP(fp) (see kernel/riscv.h. These number are helpful for backtrace to terminate its loop.
```

所以说，我只需要判断是不是 frame 地址还大于(指逻辑上大于，但是由于stack从高地址向)栈的底部就行了。 所以改成了：

```cpp
uint64 up = PGROUNDUP(frame_pointer);

  while (frame_pointer < up) 
    //Notice that the stack grow from up to down
  {...}
```

### Alarm

##### test0

这个 lab 说明书里已经写得很详细了， 一步步照做即可。

首先是上次的 lab 里已经做过好几次的流程： user/user.h 加用户所用的函数原型； user/usys.pl 加 entry， 这个脚本用于生成汇编； kernel/syscall.h 里面已经有了宏定义（映射到实数用于调用函数指针）， kernel/syscall.c 加函数指针数组的元素、 对应函数的 extern 声明、 维护 System_calls 数组。

然后和之前设计 system call 一样， 用户会把参数放在寄存器里， 然后我用内核态的函数 agrint / argaddr 拿到这些参数， 把他们放到 proc 结构体里面。


这里遇到的报错主要是忘记了 cast:

```
kernel/sysproc.c:183:21: error: assignment to 'void (*)(void)' from 'uint64' {aka 'long unsigned int'} makes pointer from integer without a cast [-Werror=int-conversion]
  183 |   myproc -> handler = handler;


kernel/trap.c:91:29: error: assignment to 'uint64' {aka 'long unsigned int'} from 'void (*)(void)' makes integer from pointer without a cast [-Werror=int-conversion]
   91 |       p -> trapframe -> epc = p -> handler;
      |                             ^
```

还有变量和函数重名了：

```
kernel/sysproc.c:171:26: error: called object 'myproc' is not a function or function pointer
  171 |   struct proc * myproc = myproc();
  ```
  
我最后写出的 system call 主函数长这样：

```cpp
uint64 
sys_sigalarm(void)
{
  struct proc * my_proc = myproc();
  int interval;
  argint(0, &interval);
  my_proc -> interval = interval;
  uint64 handler;
  argaddr(1, &handler);
  my_proc -> handler = (void(*)())handler;
  my_proc -> ticks_count = 0;
  return 0;
}
```

##### test1

这里说明书已经把实现思路写好了：用户负责给 handler 函数加上 sigreturn 函数。所以我需要思考的就是， 恢复哪些数据（寄存器、状态）以及如何恢复数据。

所以我要存的东西是什么呢？其实就是原本已经定义好的这个结构体里的东西: 

```cpp
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

```

粗略一看大概就是， kernel page table、 kernel stack pointer、 user PC、 hardware thread ID 然后还有一堆寄存器。

这里很值得仔细想一下。（我以前很喜欢粗想一想然后先写上了再仔细想，但是那样经常写成到处都是 bug 然后最后只能 git checkout 回去重新写）

首先， 我肯定要把 trapframe 备份一遍。 其次， 说明书告诉我 sigreturn 作为一个函数（返回 uint64）， 它会修改 a0 寄存器。 所以 a0 我要单独备份。

那我就在 proc 结构体里再存一个 trapframe 的指针用于备份，然后再单独存一个 a0。 相当于把结构体存内存里面， 然后进程自己来备份这些数据。 然后我把对应的 alloc / free 给写好。 然后这时我再存一个 bool (我后来发现 bool 居然是未定义类型， 那就用 char) 来表示此进程是否是 handler。

然后我每次进了 usertrap， 要把控制权交给 user space 的 handler 的时候， 我先备份 trapframe (但是我第一次还给写成了指针赋值， 应该是深拷贝才对)， 然后备份当前 a0寄存器（因为 sys_return 出来的时候不可避免改掉它）。 接下来我把 epc 设置为 handler， PC 跳到 handler, handler 做完调用 sigreturn system call。 sigreturn 把 trapframe 恢复， 但是返回的时候会改 a0，因此在 syscall() 主函数（就是那个调用函数指针的函数） 里面我判断这个 syscall 为 SYS_sigreturn 的时候专门恢复 a0。

这样我就顺利写完了这个 lab， 写得很开心。