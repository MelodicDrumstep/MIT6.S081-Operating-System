#ifndef FILE_H
#define FILE_h

#include "struct_file.h"
#include "struct_inode.h"
#include "sleeplock.h"
#include "fs.h"

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
#define STATS   2

#endif