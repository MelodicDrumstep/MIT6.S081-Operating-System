#ifndef BUF_H
#define BUF_H

//#include <stddef.h>

struct buf 
{
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *next;
  struct buf *prev;
  uchar data[BSIZE] __attribute__((aligned(4096)));
  int ticks;
};

#include "kernel/get_buf_from_data.h"

#endif