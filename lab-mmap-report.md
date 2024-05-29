这个 lab 是写一个 `mmap` 和 `munmap` 系统调用。 我之前写过只使用 `sbrk` 的 `malloc`, 写完 `mmap` 之后对内存分配的理解就会更深了。

# 前置工作

首先就是注册系统调用。原型写成这样:

```c
char * mmap(void *addr, size_t length, int prot, int flags,
           int fd, off_t offset);
int munmap(void *addr, size_t length);
```

然后其他地方注册好。 这步完成之后 `mmaptest` 就可以编译了。

