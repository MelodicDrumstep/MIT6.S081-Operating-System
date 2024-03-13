#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64 fmemory_counting(void);
uint64 fproc_counting(void);
//Add the prototype
//for the linker to search for the function

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  
  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
#ifdef LAB_TRAPS
#endif
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  //This function will modify the 
  //mask_num of the current process
  //to the value of the first argument
  int mask_num;

  argint(0, &mask_num);
  //Fetch the argument from the register a0

  myproc() -> mask_num |= mask_num;
  //Modify the mask_num of the current process
  //use "|=" because we want to add the new mask_num
  //rather than substitution

  return 0;
}

uint64
sys_sysinfo(void)
{
  uint64 pointer2user_sysinfo;
  argaddr(0, &pointer2user_sysinfo);
  //Get the pointer to the user space
  //It will store the information of system info

  struct proc * current_proc = myproc();
  //Get the current process

  struct sysinfo info;
  //Create a new sysinfo struct

  info.freemem = fmemory_counting();
  //Get the free memory  bytes

  info.nproc = fproc_counting();
  //Get the number of processes

  // printf("%d\n", info.freemem);
  // printf("%d\n", info.nproc);

  if(copyout(current_proc -> pagetable, pointer2user_sysinfo, (char *)&info, sizeof(info)) < 0)
  {
    return -1;
  }

  //The info is create inside the kernel space
  //And I have to copy it to the user space

  return 0;
}