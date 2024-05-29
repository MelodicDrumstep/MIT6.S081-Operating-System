这个 lab 是关于文件系统的。

# Large files

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

# Symbolic link

这个任务是写一个软链接的系统调用。我们已经有了一个硬链接的系统调用， 所以我们先来读懂它。

在读懂它之前， 我们先来看一些其中需要用到的函数和类的实现。

## 关于 sys_link

### inode

```c
// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT + 2];
};
```

值得注意的是， 我们的 `inode` 维护了 `refcnt` 表示引用个数， `valid` 表示是否需要从磁盘更新， `nlink` 表示链接个数。

### dinode

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

我们的 `dinode` 表示从磁盘读来的 `inode`. 我们使用它的方式是这样的:

```c
bp = bread(ip->dev, IBLOCK(ip -> inum, sb));
dip = (struct dinode*)bp->data + ip->inum % IPB;
ip->type = dip->type;
ip->major = dip->major;
ip->minor = dip->minor;
ip->nlink = dip->nlink;
ip->size = dip->size;
memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
brelse(bp);
ip->valid = 1;
if(ip->type == 0)
{
    panic("ilock: no type");
}
```

### ilock

`ilock` 函数会做两件事： 

+ 1.获取 `inode` 的 `sleeplock`. 

+ 2.检查 `inode` 是否是最新的。 如果不是， 需要从磁盘重新读。

它的实现是这样的。 我写了一些注释。

```c
// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || atomic_read4(&ip->ref) < 1)
  {
    // check if the inode is empty or refcnt < 1
    // If so, panic
    panic("ilock");
  }

  // Try to acquire the sleep lock
  // If cannot get the sleep lock, this thread will sleep
  // and be waked up when the lock is available
  acquiresleep(&ip->lock);
  
  if(ip->valid == 0)
  {
    // ip -> valid represent whether I has been read from disk or not
    // Then if not, I have to read the inode from disk
    // avoiding using the garbage inode
    bp = bread(ip->dev, IBLOCK(ip -> inum, sb));
    dip = (struct dinode*)bp->data + ip->inum % IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}
```

### iunlock

```c
// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || atomic_read4(&ip->ref) < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}
```

`iunlock` 就是释放 `sleeplock`.

### iput

```c
// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0)
  {
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);
}
```

`iput` 会减少一个 `refcnt`, 如果 `ref` 减到了 0 且没有 `link` ， 那么释放这个 `inode` 的资源。

### iunlockput

```c
// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}
```

`iunlockput` 会 `iunlock + input`.

### iupdate

```c
// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}
```

`iupdate` 会把一个内存中修改的 `inode` 写回到磁盘。

### namex

```c
// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0)
  {
    ilock(ip);
    if(ip->type != T_DIR)
    {
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0')
    {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0)
    {
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent)
  {
    iput(ip);
    return 0;
  }
  return ip;
}
```

`namex` 会寻找路径对应的 `inode` 或者路径对应的父目录的 `inode` 并返回。 如果是后一种情况， 会把文件名填入传入的参数 `name` 中。

### nameiparent

```c
struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
```

`nameiparent` 就是调用后一种情况的 `namex`.

### dirlookup

```c
// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de))
  {
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }
}
```

我们知道， 目录里面存的是 `name` 到 `inode` 的映射。 `dirlookup` 可以搜索一个目录内的 `name` 对应的 `inode`.

### dirlink

```c
// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de))
  {
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}
```

`dirlink` 可以向一个目录内添加一个 `(name, inode)` 对。

### sys_link

接下来我们就可以看 `sys_link` 系统调用的实现了。 我为这个函数写了非常详细的注释。

```c
// Create the path new as a link to the same inode as old.
// This is HARD LINK
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  // "name" stores the file name
  // "new" is the new file path
  // "old" is the old file path
  struct inode *dp, *ip;
  // dp is the pointer to the directory inode
  // ip is the pointer to the file inode

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
  {
    return -1;
  }
  // Get "old" and "new" from the argument

  begin_op();
  // Notice that we have to enclose every operation with file system
  // to be inside begin_op and end_op
  // to ensure crash safety

  if((ip = namei(old)) == 0)
  {
    end_op();
    return -1;
  }
  // Try to find the inode w.r.t. name old 
  // If failed, [end_op] and return
  // (Remember to end_op!! Otherwise this transaction will continue)

  ilock(ip);
  // Acquire the sleep lock of this inode
  // And read from disk to renew its value if needed

  if(ip->type == T_DIR)
  {
    // If this inode represent a directory
    // release the lock and end_op and return
    // HARD LINK can not be used with directory
    iunlockput(ip);
    // iunlockput will unlock the sleep lock
    // decrease a refcnt
    // and it recnt drop to 0 and link num is 0
    // release the resources of this inode
    end_op();
    return -1;
  }

  ip->nlink++;
  // add the link number
  iupdate(ip);
  // write back the inode in memory to disk
  iunlock(ip);
  // unlock the sleep lock

  if((dp = nameiparent(new, name)) == 0)
  {
    // get the parent directory inode
    // Notice that "name" has not been initialized by sys_link
    // It will be filled by nameiparent to the file name
    goto bad;
  }
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0)
  {
    // dirlink will add (name, inum) to the directory represented by dp
    // So I will store the inode fetched from another path inside this directory
    // This will create hard link because we shared the same inode
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  // error happens
  // nlink-- and write back and drop the refcnt
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}
```

## 开始 sys_symlink!

接下来我们来实现软连接 `symlink` 系统调用。

首先我们增添一种文件类型:

```c
#define T_DIR     1   // Directory
#define T_FILE    2   // File
#define T_DEVICE  3   // Device
#define T_SYMLINK 4   // SYMBOLIC LINK
```

然后给 `open` 用到的参数增添一个 bit:

```c
#define O_RDONLY   0x000
#define O_WRONLY   0x001
#define O_RDWR     0x002
#define O_CREATE   0x200
#define O_TRUNC    0x400
#define O_NOFOLLOW 0x800
// If "open" system call is enabledd with O_NOFOLLOW
// I will let "open" open the symlink (and not follow the symlink)
```

现在我需要思考， `symbolic link(soft link)` 和 `hard link` 有什么区别？ 区别在于， `hard link` 在目录下存储了真实的共享的 `inode`, 而 `symbolic link` 只在目录下存储了文件路径。 所以 `symbolic link` 是更加"间接"的。

```
注： 这部分思路一开始是错误的， 实际上我不能修改 inode 结构体的格式与大小。 后面我会在出现 bug 之后对这一思路进行修正。
```

那么我需要一个位置来存储 `symbolic link` 存储的文件路径。 我可以把 `symbolic link` 对应的文件视为一个普通文件， 然后把对应的文件路径存入这个文件吗？ 理论上可以， 但是我们这里要求除了 `open`, 其他的系统调用都是 `not follow symbolic link` 的。 它们可以修改文件内容。 另一方面， 这样存储还要访问文件 `block`, 效率不高。

那我不妨直接给 `inode` 增加一个区域， 存储这个路径好了。 所以我需要修改 `dinode` 和 `inode` 的原型。

```c
// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  char  sym_link_path;  // the path represented by the symbolic link (T_SYMLINK only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT + 2];   // Data block addresses
};
```

```c
// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?
  
  char  sym_link_path;  // the path represented by the symbolic link (T_SYMLINK only)
  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT + 2];
};
```

接下来我需要思考， `symlink` 和 `link` 在实现上哪些过程会有不同？

+ 是否创建 inode

`symlink` 需要新创建一个 `inode`, 类型为 `T_SYMLINK`, 然后把新路径存入这个 `inode` 中。 `link` 不用新建 `inode`， 只需要把 `(name, inode_num)` 存入目录即可。

按照这种思路， 我实现了 `sys_symlink` 和 `sysopen`:

```c
// Create the path new as a link to the same inode as old.
// This is SOFT LINK
uint64
sys_symlink(void)
{
  char new[MAXPATH], old[MAXPATH];
  // "new" is the new file path
  // "old" is the old file path
  struct inode *ip;
  // dp is the pointer to the directory inode
  // we will create a new inode for the link file
  // and assign it to ip.

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
  {
    return -1;
  }
  // Get "old" and "new" from the argument

  begin_op();
  // Notice that we have to enclose every operation with file system
  // to be inside begin_op and end_op
  // to ensure crash safety

  if((ip = create(new, T_SYMLINK, 0, 0)))
  {
    end_op();
    return -1;
  }

  strncpy(&(ip -> sym_link_path), old, MAXPATH);

  // Try to Create a new symbolic linked file
  // If failed, [end_op] and return
  // (Remember to end_op!! Otherwise this transaction will continue)


  ilock(ip);
  // Acquire the sleep lock of this inode
  // And read from disk to renew its value if needed

  iupdate(ip);
  // write back the inode in memory to disk
  iunlock(ip);
  // unlock the sleep lock
  iput(ip);

  end_op();

  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE)
  {
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0)
    {
      end_op();
      return -1;
    }
  } 
  else 
  {
    if((ip = namei(path)) == 0)
    {
      // Get the inode pointer and check if it's valid
      end_op();
      return -1;
    }

    // Follow the symbolic link
    for(int i = 0; i < 10; i++)
    {
      if(ip -> type == T_SYMLINK && !(omode & O_NOFOLLOW))
      {
        strncpy(path, &(ip -> sym_link_path), MAXPATH);
        // Modify the path to be the path represented by this soft link
        if((ip = namei(path)) == 0)
        {
          // Get the inode pointer and check if it's valid
          end_op();
          return -1;
        }
      }
    }

    // If ip is still symbolic link inode
    // Then I assume it's cyclic
    // then panic and return
    if(ip -> type == T_SYMLINK)
    {
      panic("Cyclic symbolic link");
      return -1;
    }

    ilock(ip);
    // Cannot ope a directory without read only
    if(ip->type == T_DIR && omode != O_RDONLY)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE)
  {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } 
  else 
  {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip -> type == T_FILE)
  {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

```

然后我 `make qemu` 发现写出了这样的 bug:

```
mkfs/mkfs fs.img README  user/_cat user/_echo user/_forktest user/_grep user/_init user/_kill user/_ln user/_ls user/_mkdir user/_rm user/_sh user/_stressfs user/_usertests user/_grind user/_wc user/_zombie  user/_bigfile user/_symlinktest
mkfs: mkfs/mkfs.c:85: main: Assertion `(BSIZE % sizeof(struct dinode)) == 0' failed.
make: *** [Makefile:264: fs.img] Aborted
```

这个报错是说， 我不能修改 `dinode, inode` 结构体的大小。 实际上这也很合理： `inode` 是按照固定格式组织在一起的， 一个 `block` 可以存多个 `inode`， 自然我不能随便改它的结构的。

那看来我的思路是有问题的。 我应该把路径直接存入这个 `T_SYMLINK` 文件内容中。 (可以就存在前几个字节)

那就用提供好的 `write_i` 和 `read_i` 函数。 它们的原型是

```c
// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n);

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n);
```

这里我使用 `writei` 和 `readi` 修改了函数， 写成了这样:

```c
// Create the path new as a link to the same inode as old.
// This is SOFT LINK
uint64
sys_symlink(void)
{
  char new[MAXPATH], old[MAXPATH];
  // "new" is the new file path
  // "old" is the old file path
  struct inode *ip;
  // we will create a new inode for the link file
  // and assign it to ip.

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
  {
    return -1;
  }
  // Get "old" and "new" from the argument

  begin_op();
  // Notice that we have to enclose every operation with file system
  // to be inside begin_op and end_op
  // to ensure crash safety

  if((ip = namei(new)))
  {
    // This means this file name already exists
    // Which leads to error state
    end_op();
    return -1;
  }

  if((ip = create(new, T_SYMLINK, 0, 0)) == 0)
  {
    // Try to Create a new symbolic linked file
    // If failed, [end_op] and return
    // (Remember to end_op!! Otherwise this transaction will continue)
    end_op();
    return -1;
  }
  ilock(ip);

  if(writei(ip, 0, (uint64)old, 0, MAXPATH) < 0)
  {
    // Use "writei" to write the old path to the first bytes inside the first block
    iunlock(ip);
    iput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  // Remember to use iunlockput to drop the refcnt and release the lock
  // at the same time
  end_op();

  return 0;
}


uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE)
  {
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0)
    {
      end_op();
      return -1;
    }
  } 
  else 
  {
    if((ip = namei(path)) == 0)
    {
      // Get the inode pointer and check if it's valid
      end_op();
      return -1;
    }

    ilock(ip);

    // Follow the symbolic link
    int i;
    for(i = 0; i < 10; i++)
    {
      if(ip -> type == T_SYMLINK && !(omode & O_NOFOLLOW))
      {
        memset(path, 0, MAXPATH);
        if(readi(ip, 0, (uint64)(path), 0, MAXPATH) <= 0)
        {
          iunlockput(ip);
          end_op();
          return -1;
        }
        // Modify the path to be the path represented by this soft link

        if((ip = namei(path)) == 0)
        {
          // Get the inode pointer and check if it's valid
          iunlockput(ip);
          end_op();
          return -1;
        }
      }
      else
      {
        break;
      }
    }

    // If ip is still symbolic link inode
    // Then I assume it's cyclic
    // then panic and return
    if(i == 10 && ip -> type == T_SYMLINK)
    {
      iunlockput(ip);
      end_op();
      panic("Cyclic symbolic link");
      return -1;
    }
    
    // Cannot ope a directory without read only
    if(ip->type == T_DIR && omode != O_RDONLY)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE)
  {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } 
  else 
  {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip -> type == T_FILE)
  {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}
```

然后成功运行了 `make qemu`. 但是此时发现 `symlinktest` 会卡死， 我猜是有死锁了。 

找了很久终于发现了问题:

```c
// RIGHT version
  if((ip = create(new, T_SYMLINK, 0, 0)) == 0)
  {
    // Try to Create a new symbolic linked file
    // If failed, [end_op] and return
    // (Remember to end_op!! Otherwise this transaction will continue)
    end_op();
    return -1;
  }
  // ilock(ip);
  // Notice !!!! create has already acquire the lock of this inode
  // SO DO NOT acquire it again!!!

// WRONG version
  if((ip = create(new, T_SYMLINK, 0, 0)) == 0)
  {
    // Try to Create a new symbolic linked file
    // If failed, [end_op] and return
    // (Remember to end_op!! Otherwise this transaction will continue)
    end_op();
    return -1;
  }
  ilock(ip);
```

然后发现这个条件写错了

```c
// RIGHT version
  if((ip = create(new, T_SYMLINK, 0, 0)) == 0)
  {
    end_op();
    return -1;
  }
// WRONG version
  if((ip = create(new, T_SYMLINK, 0, 0)))
  {
    end_op();
    return -1;
  }
```

之后出现了 `unlock panic`, 我找了很久， 发现我每次拿新的 `inode` 的时候， 没有释放前一个 `inode` 的锁并拿下一个 `inode` 的锁:

```c
// --------------------------------------
// RIGHT version
    if((ip = namei(path)) == 0)
    {
      // Get the inode pointer and check if it's valid
      end_op();
      return -1;
    }

    ilock(ip);
    //lock the first ip

    // Follow the symbolic link
    int i;
    // i represent the loop number
    for(i = 0; i < 10; i++)
    {
      if(ip -> type == T_SYMLINK && !(omode & O_NOFOLLOW))
      {
        memset(path, 0, MAXPATH);
        if(readi(ip, 0, (uint64)(path), 0, MAXPATH) <= 0)
        {
          iunlockput(ip);
          end_op();
          return -1;
        }
        // Modify the path to be the path represented by this soft link

        iunlockput(ip);
        // Notice!!! I have to unlock and put the former inode
        // Before fetching the next inode!!!
        // Otherwise it will lead to deadlock or unlock panic

        if((ip = namei(path)) == 0)
        {
          // Get the inode pointer and check if it's valid
          end_op();
          return -1;
        }

        ilock(ip);
        // Remeber to lock the branch new inode here!!
      }
      else
      {
        break;
      }
    }
// ---------------------------------------------



// ---------------------------------------------
// WRONG version
    ilock(ip);

    // Follow the symbolic link
    int i;
    for(i = 0; i < 10; i++)
    {
      if(ip -> type == T_SYMLINK && !(omode & O_NOFOLLOW))
      {
        memset(path, 0, MAXPATH);
        if(readi(ip, 0, (uint64)(path), 0, MAXPATH) <= 0)
        {
          iunlockput(ip);
          end_op();
          return -1;
        }
        // Modify the path to be the path represented by this soft link

        if((ip = namei(path)) == 0)
        {
          // Get the inode pointer and check if it's valid
          iunlockput(ip);
          end_op();
          return -1;
        }
      }
      else
      {
        break;
      }
    }
// -----------------------------------------------------
```

把这些 bug 全修完之后就顺利通过了测试。

```
$ symlinktest
Start: test symlinks
test symlinks: ok
Start: test concurrent symlinks
test concurrent symlinks: ok
```

### 完整函数

最后贴一下完成函数， 我写了详细注释:

#### sys_symlink

```c
// Create the path new as a link to the same inode as old.
// This is SOFT LINK
uint64
sys_symlink(void)
{
  char new[MAXPATH], old[MAXPATH];
  // "new" is the new file path
  // "old" is the old file path
  struct inode *ip;
  // we will create a new inode for the link file
  // and assign it to ip.

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
  {
    return -1;
  }
  // Get "old" and "new" from the argument

  begin_op();
  // Notice that we have to enclose every operation with file system
  // to be inside begin_op and end_op
  // to ensure crash safety

  if((ip = namei(new)))
  {
    // This means this file name already exists
    // Which leads to error state
    end_op();
    return -1;
  }

  

  if((ip = create(new, T_SYMLINK, 0, 0)) == 0)
  {
    // Try to Create a new symbolic linked file
    // If failed, [end_op] and return
    // (Remember to end_op!! Otherwise this transaction will continue)
    end_op();
    return -1;
  }
  // ilock(ip);
  // Notice !!!! create has already acquire the lock of this inode
  // SO DO NOT acquire it again!!!

  if(writei(ip, 0, (uint64)old, 0, MAXPATH) < 0)
  {
    // Use "writei" to write the old path to the first bytes inside the first block
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  // Remember to use iunlockput to drop the refcnt and release the lock
  // at the same time
  end_op();

  return 0;
}
```

#### sys_open

```c
uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE)
  {
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0)
    {
      end_op();
      return -1;
    }
  } 
  else 
  {
    if((ip = namei(path)) == 0)
    {
      // Get the inode pointer and check if it's valid
      end_op();
      return -1;
    }

    ilock(ip);
    //lock the first ip

    // Follow the symbolic link
    int i;
    // i represent the loop number
    for(i = 0; i < 10; i++)
    {
      if(ip -> type == T_SYMLINK && !(omode & O_NOFOLLOW))
      {
        memset(path, 0, MAXPATH);
        if(readi(ip, 0, (uint64)(path), 0, MAXPATH) <= 0)
        {
          iunlockput(ip);
          end_op();
          return -1;
        }
        // Modify the path to be the path represented by this soft link

        iunlockput(ip);
        // Notice!!! I have to unlock and put the former inode
        // Before fetching the next inode!!!
        // Otherwise it will lead to deadlock or unlock panic

        if((ip = namei(path)) == 0)
        {
          // Get the inode pointer and check if it's valid
          end_op();
          return -1;
        }

        ilock(ip);
        // Remeber to lock the branch new inode here!!
      }
      else
      {
        break;
      }
    }

    // If ip is still symbolic link inode
    // Then I assume it's cyclic
    // then return -1
    if(i == 10 && ip -> type == T_SYMLINK)
    {
      iunlockput(ip);
      end_op();
      //panic("Cyclic symbolic link");
      return -1;
    }
    
    // Cannot ope a directory without read only
    if(ip->type == T_DIR && omode != O_RDONLY)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV))
  {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0)
  {
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE)
  {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } 
  else 
  {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip -> type == T_FILE)
  {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}
```


