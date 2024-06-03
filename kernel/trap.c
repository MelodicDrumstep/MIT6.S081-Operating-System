#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"
#include "struct_file.h"
#include "struct_inode.h"

// #define DEBUG

#ifdef DEBUG
int truth = 1;
#endif
//#define DEBUG2

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  // Notice !! This get the user process control block
  // the user process is the one get into this kernel thread 
  struct proc *p = myproc();
  
  // save user program counter.
  p -> trapframe -> epc = r_sepc();
  
  if(r_scause() == 8)
  {
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
    
  } 
  else if((which_dev = devintr()) != 0)
  {
    // ok
  } 

  else if(r_scause() == 13 || r_scause() == 15)
  {
    uint64 va = r_stval();
    // Notice that now we are in user mode
    // myproc() will return the process causing

    int have_found = 0;
    struct vma * pointer_to_vma;
    int i;
    for(i = 0; i < MAX_VMA; i++)
    {
      // Search every vma inside this process to find the vma which this page 
      // belongs to
      pointer_to_vma = &(p -> vma[i]);
      if(pointer_to_vma -> used == 0)
      {
        continue;
      }
      if(va >= pointer_to_vma -> starting_addr && 
         va < pointer_to_vma -> starting_addr + pointer_to_vma -> length)
      {
        // Check if va is inside the range of some vma
        // (We can view a vma mapping to a "memory block")
        have_found = 1;
        break;
      }
    }

    if(have_found == 0)
    {
      panic("Page fault but cannot find vma corresponding to it");
    }

    // DEBUGING 
    #ifdef DEBUG
      printf("index number is: %d\n", i);
    #endif
    // DEBUGING

    struct inode * ip = pointer_to_vma -> vma_file -> ip;
    // Get the inode pointer of this file because I need to read the data
    // of the first 4KB

    va = PGROUNDDOWN(va);
    uint64 pa;

    // Below is the allocation page part
    // I don't need them for the improved version of mmap

    // if((pa = (uint64)kalloc()) == 0)
    // {
    //   panic("No available resouces to alloc");
    // }
    // // alloc the physical space
    // // Now I haven't create any mapping in the page table

    // memset((void * )pa, 0, PGSIZE);
    // initialization

    begin_op();
    // Remember to enclose every operation with inode
    // inside begin_op and end_op

    // DEBUGING 
    #ifdef DEBUG
      printf("ip -> refcnt : %d\n", ip -> ref);
    #endif
    // DEBUGING

    ilock(ip);

    uint64 read_file_offset = pointer_to_vma -> offset + (va - pointer_to_vma -> starting_addr);
    // I should read the first 4KB w.r.t va
    // So the starting point would be (va - addr_file) + offset

    /* 
    Old version
    -----------------------
    if((readi(ip, 0, (uint64)pa, read_file_offset, PGSIZE)) < 0)
    {
      // Read the data into pa
      iunlockput(ip);
      end_op();
      panic("can not read from the file");
    }
    -----------------
    */

   // For debug
    // if((readi_debug(ip, 0, (uint64)pa, read_file_offset, PGSIZE)) < 0)
    // {
    //   // Read the data into pa
    //   iunlockput(ip);
    //   end_op();
    //   panic("can not read from the file");
    // }

    // DEBUGING
    #ifdef DEBUG
      printf("I'm going to call readi_return_buf\n");
    #endif
    // DEBUGING

    // New version
    if((pa = (uint64)readi_return_buf(ip, read_file_offset)) < 0)
    {
      // Read the data into pa
      iunlockput(ip);
      end_op();
      panic("can not read from the file");
    }

    // DEBUGING
    #ifdef DEBUG
      printf("End of calling readi_return_buf, and pa as address is %p\n", pa);
    #endif
    // DEBUGING

    // iunlockput(ip);
    // At here, DO NOT use iput to drop a refcnt!!!
    // Because this inode is using along the mmap block
    // Then I should hold the refcnt along the mmap block
    iunlock(ip);
    end_op();

    // Now set the PTE flags according to the 
    // vma flags
    // Notice that we manage the permission
    // by use of PTE flags (page wide permission)
    int flags = 0;
    if(pointer_to_vma -> prot & PROT_READ)
    {
      flags |= PTE_R;
    }
    if(pointer_to_vma -> prot & PROT_WRITE)
    {
      flags |= PTE_W;
    }
    if(pointer_to_vma -> prot & PROT_EXEC)
    {
      flags |= PTE_X;
    }
    flags |= PTE_U;
    // Let the user access this page

    // Create page table mapping
    if(mappages(p -> pagetable, va, PGSIZE, (uint64)pa, flags) < 0)
    {
      kfree((void * )pa);
      panic("Mapping failed");
    }

    // DEBUGING
    #ifdef DEBUG
      printf("va is %p, pa is : %p\n", va, pa);
      printf("the first char is 0x%x\n", ((char * )pa)[0]);
      uint64 ppa = walkaddr(p -> pagetable, va);
      printf("ppa is %p\n", ppa);
      printf("the first char is 0x%x\n", ((char * )ppa)[0]);
      printf("\n");
    #endif
    // DEBUGING
  }

  else 
  {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
  {
    //When we have a timer interrupt, we have to renew 
    //the count of ticks of the process
    //And if the process has reached the limit of ticks
    //we set the return PC to be the function
    p -> ticks_count++;
    if(p -> is_handler == 0 && p -> ticks_count == p -> interval)
    {
      p -> is_handler = 1;
      p -> ticks_count = 0;
      *(p -> backup_trapframe) = *(p -> trapframe);
      p -> a0 = p -> trapframe -> a0;
      p -> trapframe -> epc = (uint64)p -> handler;
      //Must have this cast to convert pointer to a integer
    }
    yield();
    //give up the CPU 
  }

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);

}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

