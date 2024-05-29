#ifndef STRUCT_FILE_H
#define STRUCT_FILE_H

struct file {
#ifdef LAB_NET
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_SOCK } type;
#else
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
#endif
  int ref; 
  // reference count

  char readable;
  char writable;
  // permissions
  struct pipe *pipe; 
  // a pointer to pipe, only apply to file type FD_PIPE

  struct inode *ip;  
  // a pointer to inode, only apply to file type FD_INODE and FD_DEVICE

#ifdef LAB_NET
  struct sock *sock; // FD_SOCK
#endif
  uint off;          
  // offset, only apply to file type FD_INODE
  short major;       
  // the device number, only apply to file type FD_DEVICE
};

#endif