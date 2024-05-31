#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

// #define TEST_PRINT

// Initialize a buffer ring for sending
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

// Initialize a buffer ring for receiving
#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
// This function will configure E1000 to do "DMA",
// namly, read packets to be transmitted from RAM
// and write received pakcets to RAM
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) 
  {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) 
  {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

// This will be called when the host already pack a packet and 
// hold a mbuf. This function will place this mbuf in the right place
// of the transmission buffer ring and let the device know we want to transmit a packet
int
e1000_transmit(struct mbuf *m)
{
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  
  // TEST_PRINT
  #ifdef TEST_PRINT
    printf("transmit!!\n");
  #endif
  // TEST_PRINT

  acquire(&e1000_lock);
  // I have to hold the lock because multiple process may want to transmit packets
  
  int index_available_in_tx_ring = regs[E1000_TDT];
  // Ask the E1000 for the TX ring index at which
  // it's expecting the next packet. This is memory mapped register

  if(!(tx_ring[index_available_in_tx_ring].status & E1000_TXD_STAT_DD))
  {
    // Check if the former packet at this place of the buffer ring has already been transmitted
    // If not, return and raise error
    release(&e1000_lock);
    return -1;
  }

  if(tx_mbufs[index_available_in_tx_ring])
  {
    // If the former packet is still in the buffer ring, free it
    mbuffree(tx_mbufs[index_available_in_tx_ring]);
  }

  // Now I can place my mbuf here
  // Firstly, modify the descriptor 
  tx_ring[index_available_in_tx_ring].addr = (uint64)(m -> head);
  // m -> head points to the packets's content in memory
  tx_ring[index_available_in_tx_ring].length = (uint16)(m -> len);
  // m -> len is the packet length
  tx_ring[index_available_in_tx_ring].cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
  // Set the necessary flags

  // And actually store a pointer of my packet
  tx_mbufs[index_available_in_tx_ring] = m;

  // Then modify the next TX ring index, this is momory mapped register
  // And this will let E1000 know that it have to do the transmission
  regs[E1000_TDT] = (regs[E1000_TDT] + 1) % TX_RING_SIZE;

  release(&e1000_lock);

  return 0;
}

// It will take away the right mbuf
// And call net_rx to automatically unpacking the packets
static void
e1000_recv(void)
{
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  // TEST_PRINT
  #ifdef TEST_PRINT
    printf("transmit!!\n");
  #endif
  // TEST_PRINT

  // I don't have to hold the lock because I just read from the ring

  while(1) // This will be a busy loop, we want to receive the packet once it's available
  {
    int index_available_in_rx_ring = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    // Ask the E1000 for the ring index at which the next waiting received packet is located

    if(!(rx_ring[index_available_in_rx_ring].status & E1000_RXD_STAT_DD))
    {
      // Check if it's available, if not, return and raise error
      return;
    }

    // Now the packet is available, set m -> len according to the length in descriptor
    rx_mbufs[index_available_in_rx_ring] -> len = (uint32)(rx_ring[index_available_in_rx_ring].length);
    if(rx_mbufs[index_available_in_rx_ring])
    {
      // Unpacking the packet using "net_rx"
      net_rx(rx_mbufs[index_available_in_rx_ring]);
    }

    // Now I have to allocate a new mbuf for later usage
    rx_mbufs[index_available_in_rx_ring] = mbufalloc(0);
    rx_ring[index_available_in_rx_ring].addr = (uint64)(rx_mbufs[index_available_in_rx_ring] -> head);
    rx_ring[index_available_in_rx_ring].status = 0;

    // And set the regs in E1000, this will let E1000 know that we finish receiving the current packet
    regs[E1000_RDT] = index_available_in_rx_ring;
  }
}

// This is the Device Interrupt Handler
// This means E1000 use DMA to place a packet into the receive buffer ring
// And we will let the device (E1000) know that
// we have seen this interrupt, then call e1000_recv to receive the packet
void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
