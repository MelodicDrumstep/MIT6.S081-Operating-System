这个 lab 是关于文件系统的。

## Large files

首先， 我们需要修改 `inode`， 增添二级索引， 支持大文件。

当前的 `inode` 是这样的：

```c
#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses
};
```

我们需要把其中一个 `direct` 指针改成二级索引指针。

这里还有个核心函数是 `bmap (kernel/fs.c)`:

```c
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf * bp;

  if(bn < NDIRECT)
  {
    // if bn is within the direct blocks field
    if((addr = ip -> addrs[bn]) == 0)
    {
      // if the block is not allocated, allocate it
      addr = balloc(ip -> dev);
      if(addr == 0)
      {
        return 0;
      }
      ip -> addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT)
  {
    // Load indirect block, allocating if necessary.
    if((addr = ip -> addrs[NDIRECT]) == 0)
    {
      addr = balloc(ip->dev);
      if(addr == 0)
      {
        return 0;
      }
      ip->addrs[NDIRECT] = addr;
    }
    // Now addr points to the indirect block
    bp = bread(ip -> dev, addr);
    a = (uint*)bp -> data;
    if((addr = a[bn]) == 0)
    {
      // If the block is not allocated, allocate it
      addr = balloc(ip->dev);
      if(addr)
      {
        a[bn] = addr;
        log_write(bp);
        // write the indirect block back to disk
      }
    }
    brelse(bp);
    return addr;
  }
  panic("bmap: out of range");
}
```

`bmap` 会寻找这个 `inode` 的第 `bn` 个 `block` 并返回。 如果没有分配， 会为它分配物理 `block`。


我们首先修改好这几个宏:

```c
#define NDIRECT 11
#define INDEXNUM (BSIZE / sizeof(uint))
// INDEXNUM is the maximum indexes can be store in one block
#define NINDIRECT1 INDEXNUM
// The maximum block that SINGLE-INDERCT index block can represent is just INDEXNUM
#define NINDIRECT2 (INDEXNUM * INDEXNUM)
// The maximum block that DOUBLE-INDERCT index block can represent is INDEXNUM * INDEXNUM
#define MAXFILE (NDIRECT + NINDIRECT1 + NINDIRECT2)
```

然后把 `inode` 和 `dinode` 的结构体数组长度改一下(因为改了 `NDIRECT` 宏):

```c
// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT + 2];   // Data block addresses
};
```

然后改写 `bmap`, 只需要仿照原来的一层 index 来写就可以了:

```c
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf * bp;

  // This is the case where the block we search for 
  // is within the direct block
  if(bn < NDIRECT)
  {
    // if bn is within the direct blocks field
    if((addr = ip -> addrs[bn]) == 0)
    {
      // if the block is not allocated, allocate it
      addr = balloc(ip -> dev);
      if(addr == 0)
      {
        return 0;
      }
      ip -> addrs[bn] = addr;
    }
    return addr;
  }
  // Remember to subtract NDIRECT from bn
  bn -= NDIRECT;

  // This is the case where the block we search for 
  // is within the SINGLE indirect block
  if(bn < NINDIRECT1)
  {
    // Load indirect block, allocating if necessary.
    if((addr = ip -> addrs[NDIRECT]) == 0)
    {
      addr = balloc(ip->dev);
      if(addr == 0)
      {
        return 0;
      }
      ip->addrs[NDIRECT] = addr;
    }
    // Now addr is indirect block
    bp = bread(ip -> dev, addr);
    a = (uint*)bp -> data;
    if((addr = a[bn]) == 0)
    {
      // If the block is not allocated, allocate it
      addr = balloc(ip->dev);
      if(addr)
      {
        a[bn] = addr;
        log_write(bp);
        // write the indirect block back to disk
      }
    }
    brelse(bp);
    return addr;
  }

  bn -= NINDIRECT1;

  // This is the case where the block we search for 
  // is within the DOUBLE indirect block
  // How to decide the 2 indexes? 
  // I just choose the naive approch: 
  // 1. bn / INDEXNUM and 2. bn % INDEXNUM
  if(bn < NINDIRECT2)
  {
      // Load indirect block, allocating if necessary.
    if((addr = ip -> addrs[NDIRECT + 1]) == 0)
    {
      addr = balloc(ip->dev);
      if(addr == 0)
      {
        return 0;
      }
      ip -> addrs[NDIRECT + 1] = addr;
    }
    // Now addr is the first indirect block
    bp = bread(ip -> dev, addr);
    a = (uint*)bp -> data;
    // Use bn / INDEXNUM as the first index
    if((addr = a[bn / INDEXNUM]) == 0)
    {
      // If the block is not allocated, allocate it
      addr = balloc(ip->dev);
      if(addr == 0)
      {
        return 0;
      }
      a[bn / INDEXNUM] = addr;
      log_write(bp);
      // write the indirect block back to disk
    }
    brelse(bp);
    // Now addr is the second indirect block
    bp = bread(ip -> dev, addr);
    a = (uint*)bp -> data;
    // use bn % INDEXNUM as the second index
    if((addr = a[bn % INDEXNUM]) == 0)
    {
      // If the block is not allocated, allocate it
      addr = balloc(ip->dev);
      if(addr == 0)
      {
        return 0;
      }
      a[bn % INDEXNUM] = addr;
      log_write(bp);
      // write the indirect block back to disk
    }

    brelse(bp);
    return addr;
  }
  panic("bmap: out of range");
}
```

然后再改写一下 `trunc` 函数， 把二级索引对应的 `block` 也全删了：

```c
// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++)
  {
    if(ip->addrs[i])
    {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  // If the SINGLE index block is not empty, clear the blocks it points to 
  if(ip -> addrs[NDIRECT])
  {
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT1; j++)
    {
      if(a[j])
      {
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
  
  // If the DOUBLE index block is not empty, clear the blocks it points to 
  // This is just a naive enumerate approach
  if(ip -> addrs[NDIRECT + 1])
  {
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < INDEXNUM; j++)
    {
      if(a[j])
      {
        struct buf * bp2 = bread(ip->dev, a[j]);
        // SHOULD not use bp here! bp has not been released
        uint * a2 = (uint*)bp2 -> data;
        for(int k = 0; k < INDEXNUM; k++)
        {
          if(a2[k])
          {
            bfree(ip -> dev, a2[k]);
          }
        }
        brelse(bp2);
        bfree(ip -> dev, ip -> addrs[NDIRECT + 1]);
        ip -> addrs[NDIRECT + 1] = 0;
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}
```


这样就顺利通过了第一个测试：

```
$ bigfile
..................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................
wrote 65803 blocks
```
