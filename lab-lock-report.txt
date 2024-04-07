### Memory Allocator


这里的任务就是， 原先是memory allocator维护一个free list， 装着所有可以被分配的空闲页。 
这样的话多个CPU来分配内存就会产生竞争。现在我们希望改成memory allocator对每个CPU都维护一个free list。 
CPU分配内存的时候先拿属于自己的那个free list里面的page, 如果自己的空了再去拿其他人的。 这样可以减少竞争。

这个实现起来很直白， 就改数据结构就行了， 改成

```cpp
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];
```

再改一改加锁解锁。 顺利完成通过测试。

### buffer cache

我一开始对这里面的bcache.buf十分疑惑：既然都链式存储了， 后面也没用到buf, 都是通过链表访问， 那这个 buf 是干嘛的呢？ 
后来我看书才知道这里的buf相当于把那些真实存在的buf放在这里， 然后维护buf的head / tail串成一个链表。 

我这里要做的工作就是， 原来的buffer cache是用一把大锁， 我这里需要减小锁的颗粒度， 从而提高并发性能， 减少竞争。

来看看优化前会出什么样的性能问题：

我们每次bget的时候， 在遍历整个链表的过程中， 都要持续保持大锁是锁住的状态。 这明显会造成堵塞。
然后这里的替换策略是LRU， 原来的实现方式是release的时候维护链表的顺序， 但是这也是要一直锁住大锁的。这就是硬堵塞了。
所以我的实现删掉了维护链表顺序的操作， 而是记载每个buf的最后访问时间， LRU的时候直接遍历一遍找出应该替换的元素即可。
这里我做的实际上就是， 我把一个单链表划分成多个单链表， 然后用hashmap把每个元素映射到它属于的单链表。

所给的buf是这样的：

```cpp
struct buf {

  int valid;   // has data been read from disk?
  //如果它是0， 说明这块缓存的内容是脏的， 需要从disk读。

  int disk;    // does disk "own" buf?
  
  uint dev;
//device number
  uint blockno;
  //block number
  
  struct sleeplock lock; //一个sleep lock

  uint refcnt; //refcnt,表示有多少个操作正在用这个buf。 为0的时候才可以evict.

  struct buf *next;
  struct buf *prev;//这俩指针用于构建双向链表。

  uchar data[BSIZE];//真正的data 区域。

  int ticks; //我新加的成员， 表示最后修改的时间戳

};
```

数据结构是这样的：

```cpp
struct BUCKET

{

  struct buf head;

  struct spinlock bucket_lock;

};//BUCKET contains a lock and a header node of struct buf

  

struct {

  struct spinlock master_lock;

  struct buf buf[NBUF];

  struct BUCKET buckets[BUCKET_SIZE];

} bcache;

//Now bcache contain some buckets of linked list
```


这里的哈希函数我写的是一个朴素的哈希：

```cpp
inline int hash(uint dev, uint blockno)

{

  return (dev + blockno) % BUCKET_SIZE;

}
```

对于binit, 就初始化大锁和每个bucket的小锁，然后初始化好bucket 的头结点就行了。

对于brelse, 就先拿到对应桶的锁， 然后 b -> refcnt = 0即可。 
这里如果refcnt == 0了也不用即使从桶里移走， 我把这个任务交给bget（Lazy的思想）

对于bunpin， 就是先拿到对应桶的锁然后refcnt--， bpin就是把bunpin对应部分改成refcnt++。

核心函数就是bget，bget有很多种情况

首先我刚进bget的时候要拿对应Bucket的bucket lock
我先遍历我的bucket 里面的buffer ,如果找到了， 更新它的ticks 和 refcnt, 然后释放bucket锁， 去拿这个buf的sleep lock， 拿到了就返回。
如果没找到， 就是Buffer miss, 那我这么做：

1.先去同一个bucket里面找LRU且refcnt == 0的buf. 
Evict它换成我要的那个page的信息（dev, blockno）更新ticks 和 refcnt， 再把buf 的valid bit设置为0. 
（这表示它目前的内容是脏的， 如果需要它的内容， 要disk IO拿到page放进缓存池中）


2.如果上述过程找不到任何一个可用的buf, 那我要么从其他链表偷， 要么从未分配到任何一个桶的buf里面拿。
我遍历整个buf数组， 找到evictable的buf。 如果它的ticks区域不是初始值， 即它在某个bucket里面，那我把它偷过来装自己新的page. 
具体而言就是， 我修改它的信息（dev, blockno, ticks）, refcnt = 1, valid = 0, 然后更新它的位置， 从之前的链表里面删掉， 再加到现在哈希到的链表头部。 

3.如果2中发现它的ticks是初始值， 说明它是一个free buf, 不再任何桶的链表里面。  那我直接把它拿过来装我的新的page. 

最后我写了一些针对corner case的维护代码。 比如最后加了个， 如果一个可行的buf都找不到那就递归找。

### 难点

最开始写出了kernel trap:


```shell
xv6 kernel is booting

hart 1 starting
hart 2 starting
scause 0x000000000000000d
sepc=0x000000008000278c stval=0x0000000000000048
panic: kerneltrap
backtrace:
0x0000000080006480
0x00000000800020bc
0x00000000800054b4
0x0000000080002d0c
0x0000000080001064
QEMU: Terminated
make: warning:  Clock skew detected.  Your build may be incomplete.
```


后来发现是一个地方的prev指针写成head指针了。 改完之后再运行，又出现了“panic: bget : no buffers”

```shell
xv6 kernel is booting

hart 1 starting
hart 2 starting
panic: bget: no buffers
backtrace:
0x0000000080006480
0x0000000080002806
0x0000000080002d10
0x0000000080001064
QEMU: Terminated
make: warning:  Clock skew detected.  Your build may be incomplete.
```

重构了代码，之后又是报acquire的错：

```shell
xv6 kernel is booting

hart 2 starting
hart 1 starting
panic: acquire
backtrace:
0x00000000800064a0
0x0000000080006998
0x0000000080002674
0x0000000080002754
0x0000000080002814
0x0000000080002d2e
0x0000000080001064
QEMU: Terminated
```

然后这个BUG也找到了， 是for循环条件写错了

改好之后又有新bug。。。booting死循环了

```shell

xv6 kernel is booting

hart 2 starting
hart 1 starting
```

中途发现很多地方都没有更新b -> ticks
最后发现bug在于我根本没在bget里面更新 buf 对应的ticks, 导致ticks 全都是初始值-1....

最终我改好了所有Bug, 完成了这个lab。 这个lab还是挺有意思的。