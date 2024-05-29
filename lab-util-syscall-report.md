
## CS2953 操作系统 Lab1 报告

##### JOHN班-涂文良

### Sleep

这一部分都是来写用户函数，也就是运行在用户态和用户空间的部分。 这里我不直接调用 system call, 而是用已经写好的衔接接口，也就是 system call stub。

Sleep.c 的实现很简单，直接调用 system call stub : sleep( ) 就行了， 然后写一下异常判断。

实际上，阅读大致架构可知，用户态里面 sleep 这个 system call 的接口，是由 "user/usys.pl" 这个脚本来生成了对应汇编， 然后进行了一些参数配置， 从而来让内核态能够选取对应的 system call 的函数指针。

### pingpong

pingpong.c 主要用 pipe 实现， 关键是把对应的 file descriptor 设置好。这个也不难。

### system call tracing

这个 trace system call 的实现我思考了许久。 我最初想的是，调用 trace 了之后，为那些被 trace 的system call 的函数加一个特殊参数，表示它正在被监控。 但是仔细想想这样是做不到的，或者说就算做到了也开销很大。 因为这样需要修改所有 system call 的函数原型。

于是后来我想到可以不修改被调用的system call 的函数原型。 由于它们被 syscall.c 里面的 syscall 用函数指针的形式调用，我直接改 syscall 就好了。 我只需要在那些被 trace 的 system call 返回之后， 输出一些信息。 那就在 syscall 里的函数指针调用之后添加一个条件判断就好了：

首先判断是不是正在被 trace, 如果是， 我现在也拿到了返回值，我直接拿这些信息输出就好了。代码如下：


```cpp
p->trapframe->a0 = syscalls[num]();

if((p -> mask_num & (1 << num)) != 0)
{
  printf("%d: syscall %s -> %d\n", p -> pid, System_calls[num], p -> trapframe -> a0);
  //Syste_calls is an array created by me
  //and p -> trapframe -> a0 is the return value of the system call
}
```

##### 踩过的坑

最开始忘了在 sys_call.h 里面添加函数指针。 另外这里的这段代码我看了很久才懂：

```cpp
static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
...
```
实际上这里是在声明 + 初始化一个函数指针的数组。 这个数组名字叫 syscalls, 里面的元素都是函数指针， 每个指针指向的函数都是参数为 void 、 返回值为 uint64。

然后在花括号里， 这里的语法是， 把第 x 位的元素设置成 y。

比如， 第一行的意思是把第 SYS_fork 位的元素设置成函数指针 sys_fork。 而在 syscall.h 里面已经把这些宏（如 Sys_fork） 定义好了。所以实际上它做的就是初始化。


### Sysinfo

sysinfo 这个 system call 是这次 lab 卡我最久的一个。

这个 sysinfo 的实现关键是， 首先我需要内核态能够拿到 freemem 和 nproc 这两个信息，而这两个信息没有直接显式存储在某个地方， 需要自己增添函数。 这两个函数都可以通过遍历数组 (proc 和 kmem.freelist ) 来实现。其次是， 我算出的这两个信息也存在内核态， 而我要把它从内核态运输到用户态， 放置到用户提供的那个地址上， 就需要用已经实现好的函数 copyout。

##### 踩过的坑

第一个坑是

一开始写好了之后，运行 make grade， 然后有如下报错：

![](https://notes.sjtu.edu.cn/uploads/upload_0c66cd99fe25c7ea41d3f4893f872bc7.png)

似乎是引起了 kernel trap。但通过报错信息我完全不知道是哪里写错了。于是我把一部分代码注释掉， 然后加 printf， 来尝试输出一下 freemem 和 nproc.

有些尝试的代码无法编译，其中一个可编译的版本也迎来了如下报错：

![](https://notes.sjtu.edu.cn/uploads/upload_eb8233cdcb1d94337fbaa1ec4aa94d99.png)

最后我也没有其他办法，只能肉眼 debug。 所幸过了一会就发现了，原来是有个很粗心的错误， 调用 myproc 函数的时候没有加括号，写成了 

```cpp
    struct proc * myproc = myproc;
```

而且不巧的是这段代码可以通过编译，之后可能造成了严重的混淆。修复这个 bug 了之后，我再次运行，又有如下 bug:

![](https://notes.sjtu.edu.cn/uploads/upload_57664cccc01cdb450ca07e061e6a8b7c.png)

这里出现的错误我一时没有找到原因，起初完全没理解这句话的意思

    sysinfo succeeded with bad argument
    
想了好一会才知道这里指的是， 我的 sysinfo 系统调用在用户提供的参数不合法的时候也没有报错，而是正常继续了。所以实际上就是我没有正确处理异常参数。

接下来就好办了，我在 sys_sysinfo 函数中加入返回值判断， 如果用户提供的内存地址不合法 （也就是 copyout 失败）， 那就报错（返回 -1 ）

代码更改如下：

``` cpp
if(copyout(current_proc -> pagetable, pointer2user_sysinfo, (char *)&info, sizeof(info)) < 0)
{
return -1;
}
```

最终我便完成了这次 lab. 通过这个 lab， 我学习到了 syscall 的实现与本质。 纸上得来终觉浅， 绝知此事要躬行。