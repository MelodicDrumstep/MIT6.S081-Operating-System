#ifndef GET_BUF_H
#define GET_BUF_H

#include <stddef.h>

struct buf * get_buf_from_data(uchar *p_data);

void Invalidate_buf(struct buf *);

#endif