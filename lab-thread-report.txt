### Uthread

这个任务是实现一个用户态的线程库， 也就是用户可以用它创造用户态线程。

我最开始以为这里实现的是线程池 ， 然而chatgpt说不是的。chatgpt说这个没有任务队列的实现， 只能算是用户级别线程的普通调度器， 不能算使用线程池。

对于uthread, 这个任务很简单。首先我把uthread_switch写好， 写法就和内核态的swtch.S一样， 把寄存器信息存到 old thread context里面， 然后把new thread context 的寄存器信息读到我们的寄存器里面。 （先store 后 load）

我只需要给struct thread这个结构体加一块存储寄存器信息的区域就可以：

```c
struct thread_registers

{

  uint64 ra;                    //registers

  uint64 sp;

  uint64 s0;

  uint64 s1;

  uint64 s2;

  uint64 s3;

  uint64 s4;

  uint64 s5;

  uint64 s6;

  uint64 s7;

  uint64 s8;

  uint64 s9;

  uint64 s10;

  uint64 s11;

};

  

struct thread

{

  char       stack[STACK_SIZE]; /* the thread's stack */

  int        state;             /* FREE, RUNNING, RUNNABLE */

  struct thread_registers regs;

};
```

然后thread_schedule 中我调用thread_switch:

```c
  if (current_thread != next_thread) {         /* switch threads?  */

    next_thread->state = RUNNING;

    t = current_thread;

    current_thread = next_thread;

    /* YOUR CODE HERE

     * Invoke thread_switch to switch from t to next_thread:

     * thread_switch(??, ??);

     */

    thread_switch((uint64)&(t -> regs), (uint64)&(next_thread -> regs));
}
```

thread_create中我清理好线程的栈和寄存器信息， 然后把stack pointer设置到栈顶， 把return address 寄存器设置为传入的函数指针。

```c
void

thread_create(void (*func)())

{

  struct thread *t;

  

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {

    if (t->state == FREE) break;

  }

  t -> state = RUNNABLE;

  // YOUR CODE HERE

  

  //firstly, I have to clear the stack and register field of this thread

  memset((void *)&(t -> stack), 0, STACK_SIZE);

  memset((void *)&(t -> regs), 0, sizeof(struct thread_registers));

  

  t -> regs.sp = (uint64)(&(t -> stack[STACK_SIZE])); //set the stack pointer to the top of the stack

  t -> regs.ra = (uint64)func; //set the return address to the function pointer

}
```

#### 梳理流程

现在我来梳理一下这里究竟发生了什么， 为什么用户态这么写个函数就没经内核态接手就自己实现多线程了。 实际上是这样的：由于我们可以用汇编直接和寄存器交互， 所以 context switch 并不是只有内核态才能做的事情。

仔细观察 uthread_switch.S 就可以发现，我们这里做的就是， 把目前CPU的寄存器都存入 old thread context里面， 然后把内存里的new thread context的寄存器信息都读到CPU寄存器里面。 这就是 context switch。

我们的有全局变量 all_thread， 作为一个数组， 表示最大线程数就是这么多， 有这些编号的线程可以用。 不过这和线程池不一样， 并不是初始化了这么多活跃的线程， 只是开了个数组存储这些信息罢了。 thread_init函数会初始化0号线程（主线程）， 设置为RUNNING。

thread_create函数从 all_thread 数组里面找到一个FREE 的地方， 表示这里没有存储实际的正在跑的线程。 那我就拿这里来存储我的新线程。 我把state设置为RUNNABLE（可被调度成为RUNNING）， 然后把之前遗留的信息全都清理干净（包括寄存器信息、栈信息）。 然后我把stack pointer设置为栈顶， 把ra(return address register)设置为传入给thread_create的那个函数指针， 之后一旦被调度， 进入 thread_switch， 就会ret到这个函数指针执行这个函数。

thread_yield 就是把当前线程current_thread(一个全局变量)的状态设置为RUNNABLE, 然后调用thread_schedule表示放弃对CPU的占用。（所以这个是主动yield的调度， 而不是强制的调度）

thread_schedule是我们最重要的函数。 它首先要找好next_thread， 即找一个RUNNABLE的线程（这里采用的就是遍历搜索， 但是注意从current_thread + 1开始找，免得找到之前的同一个线程了）。找到之后， 我们把next_thread设置为RUNNING, 然后调用thread_switch从之前的 current_thread 切换到现在的current_thread（即next_thread）。 在thread_switch内部，就会进行旧寄存器信息的store, 新寄存器信息的load， 以及最后ret到设置好的函数指针开始新线程的执行。

#### 诡异的BUG：

我最初写完了运行，终端显示非常奇怪地进入了usertrap.

然而，我只要把struct thread 从

```c
struct thread

{

  char       stack[STACK_SIZE]; /* the thread's stack */

  int        state;             /* FREE, RUNNING, RUNNABLE */

  struct thread_registers regs;

};
```

改成

```c
struct thread

{

  struct thread_registers regs;
  
  char       stack[STACK_SIZE]; /* the thread's stack */

  int        state;             /* FREE, RUNNING, RUNNABLE */

};
```

这个问题就立刻解决了。 然后我展开排查， 终于发现了问题：

问题就是因为我thread_switch是这么写的：

```c
thread_switch((uint64)t, (uint64)next_thread);
```

我这里给thread_switch传入的是 t 和 next_thread 两个 struct thread * 。但是thread_swicth应该接受存储一堆寄存器信息的地址才对。

所以正确写法应该是：

```c
thread_switch((uint64)&(t -> regs), (uint64)&(next_thread -> regs));
```

### Using thread

就是调用pthread库写一个线程安全的程序。 我直接给每个hash table bucket开一个锁， 然后每次get / put 先锁住， 执行完再解锁。

### barrier

就是实现用户程序的一个函数，相当于C++多线程的join。 对于一轮线程， 如果它调用barrier, 它就会等待同一轮的线程执行完再一起往下走。

这里就直接用条件变量来实现, 等待的时候cond_wait， 然后这一轮所有线程都做完了之后， cond_broadcast让这一轮所有等待的线程都重新往下走。
