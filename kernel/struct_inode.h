#ifndef STRUCT_INODE_H
#define STRUCT_INODE_H

#include "sleeplock.h"
#include "fs.h"

// in-memory copy of an inode
struct inode 
{
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

#endif