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
  backtrace();
  //Add the backtrace function here to backtrace the call of functions
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
uint64 
sys_pgaccess(void)
{
  uint64 start_addr;
  argaddr(0, &start_addr);
  int num;
  argint(1, &num);
  uint64 address;
  argaddr(2, &address);
  
  unsigned int mask = 0x0;
  struct proc * my_proc = myproc();
  pagetable_t * pagetable = &(my_proc -> pagetable);
  for(int i = 0; i < num; i++)
  {
    pte_t * pte = walk(*pagetable, start_addr + i * PGSIZE, 0);
    //use walk to find the page table entrys
    if(pte == 0)
    {
      return -1;
    }
    if(*pte & PTE_A)
    {
      mask |= (1 << i);
      //This means this page is accessed
    }
    *(pte) &= ~PTE_A;
    //erase the PTE_A bit(I just check, this is not access)
  }
  copyout(*pagetable, address, (char *)&mask, sizeof(mask));
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

uint64 
sys_sigalarm(void)
{
  struct proc * my_proc = myproc();
  int interval;
  argint(0, &interval);
  my_proc -> interval = interval;
  uint64 handler;
  argaddr(1, &handler);
  my_proc -> handler = (void(*)())handler;
  my_proc -> ticks_count = 0;
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc * my_proc = myproc();
  if(my_proc -> is_handler)
  {
    my_proc -> is_handler = 0;
    *(my_proc -> trapframe) = *(my_proc -> backup_trapframe);
    my_proc -> ticks_count = 0;
  }
  return 0;
}

