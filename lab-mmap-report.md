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

