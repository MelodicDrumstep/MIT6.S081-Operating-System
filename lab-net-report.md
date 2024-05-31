这个 lab 是关于操作系统和外设交互并处理中断的。 具体而言， 这里我们需要和 (qemu 模拟的) 网卡交互， 完成发包与收包。

由于我们的计算机网络课程放在大三上学期， 在写这个 lab 的时候我并没有系统性学过计算机网络， 因此对有些网络概念可以理解并不深入。

这里的核心思想是 `unix 中一切皆文件`。 我们和外设交互的时候使用的仍然是文件描述符。

# 前情提要

## packets

`qemu` 会将模拟的收发包存入 `packets.pcap` 文件中。 我可以在本机上使用这个命令查看这个包的信息:

```
root:/mnt/d/CS2953-2024# tcpdump -XXnr packets.pcap
reading from file packets.pcap, link-type EN10MB (Ethernet), snapshot length 65536
```

这里的 `tcpdump` 是一个强大的命令行网络数据包分析工具， 可以用于捕获和分析网络流量。 具体而言， 这里是参数是:

+ -X ： 以 16 进制显示数据包内容

+ -n : 不将地址转换为主机名（禁用 DNS 反向解析）， 直接显示 IP 地址和端口号。

+ -r packets.pcap : 从 `packets.pcap` 文件中读取数据包， 而不是从网络接口捕获数据包。

## network stack

`kernel/net.h` 和 `kernel/net.c` 实现了一个简易的网络协议栈， 包含 `IP, UDP, ARP`.   下面我简要介绍一下这里提供的接口。

### mbuf

`kernel/net.h` 还为我们实现了一个环形缓冲区的原型和相应函数:

```c
struct mbuf 
{
  struct mbuf  *next; // the next mbuf in the chain
  char         *head; // the current start position of the buffer
  unsigned int len;   // the length of the buffer
  char         buf[MBUF_SIZE]; // the backing store
};

char *mbufpull(struct mbuf *m, unsigned int len);
char *mbufpush(struct mbuf *m, unsigned int len);
char *mbufput(struct mbuf *m, unsigned int len);
char *mbuftrim(struct mbuf *m, unsigned int len);

// The above functions manipulate the size and position of the buffer:
//            <- push            <- trim
//             -> pull            -> put
// [-headroom-][------buffer------][-tailroom-]
// |----------------MBUF_SIZE-----------------|
//
// These marcos automatically typecast and determine the size of header structs.
// In most situations you should use these instead of the raw ops above.
#define mbufpullhdr(mbuf, hdr) (typeof(hdr)*)mbufpull(mbuf, sizeof(hdr))
#define mbufpushhdr(mbuf, hdr) (typeof(hdr)*)mbufpush(mbuf, sizeof(hdr))
#define mbufputhdr(mbuf, hdr) (typeof(hdr)*)mbufput(mbuf, sizeof(hdr))
#define mbuftrimhdr(mbuf, hdr) (typeof(hdr)*)mbuftrim(mbuf, sizeof(hdr))

struct mbuf *mbufalloc(unsigned int headroom);
void mbuffree(struct mbuf *m);

struct mbufq {
  struct mbuf *head;  // the first element in the queue
  struct mbuf *tail;  // the last element in the queue
};

void mbufq_pushtail(struct mbufq *q, struct mbuf *m);
struct mbuf *mbufq_pophead(struct mbufq *q);
int mbufq_empty(struct mbufq *q);
void mbufq_init(struct mbufq *q);
```

### byte ordering

之后是一些 byte ordering 的支持函数:

```c
// endianness support
//

static inline uint16 bswaps(uint16 val)
{
  return (((val & 0x00ffU) << 8) |
          ((val & 0xff00U) >> 8));
}

static inline uint32 bswapl(uint32 val)
{
  return (((val & 0x000000ffUL) << 24) |
          ((val & 0x0000ff00UL) << 8) |
          ((val & 0x00ff0000UL) >> 8) |
          ((val & 0xff000000UL) >> 24));
}
```

### 协议栈的数据头包

#### Ethernet

以太网头包定义如下: 存了源地址、目标地址和以太网类型(如 IP 或者 ARP)。

```c
#define ETHADDR_LEN 6

// an Ethernet packet header (start of the packet).
struct eth {
  uint8  dhost[ETHADDR_LEN];
  uint8  shost[ETHADDR_LEN];
  uint16 type;
} __attribute__((packed));

#define ETHTYPE_IP  0x0800 // Internet protocol
#define ETHTYPE_ARP 0x0806 // Address resolution protocol
```

#### IP

接下来是 IP 头包

```c
// an IP packet header (comes after an Ethernet header).
struct ip {
  uint8  ip_vhl; // version << 4 | header length >> 2
  uint8  ip_tos; // type of service
  uint16 ip_len; // total length
  uint16 ip_id;  // identification
  uint16 ip_off; // fragment offset field
  uint8  ip_ttl; // time to live
  uint8  ip_p;   // protocol
  uint16 ip_sum; // checksum
  uint32 ip_src, ip_dst;
};

#define IPPROTO_ICMP 1  // Control message protocol
#define IPPROTO_TCP  6  // Transmission control protocol
#define IPPROTO_UDP  17 // User datagram protocol

#define MAKE_IP_ADDR(a, b, c, d)           \
  (((uint32)a << 24) | ((uint32)b << 16) | \
   ((uint32)c << 8) | (uint32)d)
```

存了很多内容， 由于我没系统学过 IP 协议， 所以这里只能查阅资料列举一下:

+ ip_vhl：版本和头部长度。

+ ip_tos：服务类型。

+ ip_len：总长度。

+ ip_id：标识。

+ ip_off：片偏移字段。

+ ip_ttl：生存时间。

+ ip_p：协议类型（例如 ICMP、TCP、UDP）。

+ ip_sum：校验和。

+ ip_src：源 IP 地址。

+ ip_dst：目的 IP 地址。

#### UDP

接下来是 UDP 头包:

```c
// a UDP packet header (comes after an IP header).
struct udp {
  uint16 sport; // source port
  uint16 dport; // destination port
  uint16 ulen;  // length, including udp header, not including IP header
  uint16 sum;   // checksum
};
```

存储了这些内容:

+ sport：源端口。

+ dport：目的端口。

+ ulen：长度（包括 UDP 头部，不包括 IP 头部）。

+ sum：校验和。

#### ARP

接下来是 ARP 头包:

```c
// an ARP packet (comes after an Ethernet header).
struct arp {
  uint16 hrd; // format of hardware address
  uint16 pro; // format of protocol address
  uint8  hln; // length of hardware address
  uint8  pln; // length of protocol address
  uint16 op;  // operation

  char   sha[ETHADDR_LEN]; // sender hardware address
  uint32 sip;              // sender IP address
  char   tha[ETHADDR_LEN]; // target hardware address
  uint32 tip;              // target IP address
} __attribute__((packed));

#define ARP_HRD_ETHER 1 // Ethernet

enum {
  ARP_OP_REQUEST = 1, // requests hw addr given protocol addr
  ARP_OP_REPLY = 2,   // replies a hw addr given protocol addr
};
```

这里存储了这些内容:

+ hrd：硬件地址格式。

+ pro：协议地址格式。

+ hln：硬件地址长度。

+ pln：协议地址长度。

+ op：操作（请求或回复）。

+ sha：发送者的硬件地址。

+ sip：发送者的 IP 地址。

+ tha：目标硬件地址。

+ tip：目标 IP 地址。

#### DNS

接下来是 DNS 头包。

```c
// an DNS packet (comes after an UDP header).
struct dns {
  uint16 id;  // request ID

  uint8 rd: 1;  // recursion desired
  uint8 tc: 1;  // truncated
  uint8 aa: 1;  // authoritive
  uint8 opcode: 4; 
  uint8 qr: 1;  // query/response
  uint8 rcode: 4; // response code
  uint8 cd: 1;  // checking disabled
  uint8 ad: 1;  // authenticated data
  uint8 z:  1;  
  uint8 ra: 1;  // recursion available
  
  uint16 qdcount; // number of question entries
  uint16 ancount; // number of resource records in answer section
  uint16 nscount; // number of NS resource records in authority section
  uint16 arcount; // number of resource records in additional records
} __attribute__((packed));

struct dns_question {
  uint16 qtype;
  uint16 qclass;
} __attribute__((packed));
  
#define ARECORD (0x0001)
#define QCLASS  (0x0001)

struct dns_data {
  uint16 type;
  uint16 class;
  uint32 ttl;
  uint16 len;
} __attribute__((packed));
```

存储了这些内容:

+ id：请求 ID，用于匹配查询和响应。

+ rd：递归查询请求位。如果设置，表示请求递归查询。

+ tc：截断位。如果设置，表示响应被截断。

+ aa：权威应答位。如果设置，表示服务器是权威服务器。

+ opcode：操作码，表示查询的类型（标准查询、反向查询、服务器状态请求）。

+ qr：查询/响应位。0 表示查询，1 表示响应。

+ rcode：响应代码，表示响应状态（例如，成功、格式错误、服务器失败、名称错误）。

+ cd：禁用校验位。如果设置，表示禁用 DNSSEC 校验。

+ ad：已认证数据位。如果设置，表示响应经过 DNSSEC 认证。

+ z：保留位，目前未使用。

+ ra：递归可用位。如果设置，表示服务器支持递归查询。

+ qdcount：查询问题的数量。
+ ancount：回答部分的资源记录数。
+ nscount：权威部分的资源记录数。
+ arcount：额外部分的资源记录数。

#### 收发包函数

同时 `kernel/net.c` 中实现了收发这些包的函数， 如

```c
// sends a UDP packet
void
net_tx_udp(struct mbuf *m, uint32 dip,
           uint16 sport, uint16 dport);

// receives an IP packet
static void
net_rx_ip(struct mbuf *m);
```

## PCI & Memory Mapped IO

`kernel/pci.c` 包含了在 `PCI bus` 上寻找 E1000 网卡的代码。 大致过程就是， 遍历 bus0 上的所有 PCI 设备， 检查 ID 是否是 E1000 对应 ID。 如果符合， 设置对应状态寄存器。  

这里所说的 `PCI bus` 是什么？ 

`PCI` 全称为 `Peripheral Component Interconnect`. `PCI` 总线可以把外围设备 (比如网卡、声卡、显卡、存储控制器) 连接到计算机主板。

这里需要注意， 我们使用了 `memory mapped IO` 技术。 具体而言， 我们将 e1000 的寄存器放置在了内存中的一个特殊位置:

```c
void
pci_init()
{
  // we'll place the e1000 registers at this address.
  // vm.c maps this range.
  uint64 e1000_regs = 0x40000000L;

  // qemu -machine virt puts PCIe config space here.
  // vm.c maps this range.
  uint32  *ecam = (uint32 *) 0x30000000L;
  //...
}
```

在 `kernel/vm.c` 中可以看到， 它是一个直接映射:

```c
// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
    //...
    #ifdef LAB_NET
    // PCI-E ECAM (configuration space), for pci.c
    kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

    // pci.c maps the e1000's registers here.
    kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
    #endif  
    //...
}
```

这看上去有点奇怪: 设备的寄存器， 为什么存储位置在物理内存上？ 实际上， 这里只是将设备的存储空间映射到这个物理内存， 这样我可以使用访问内存的语法来同样访问设备的存储空间。有一些元件会负责将访问这块物理内存的指令转化为访问设备的存储空间。

## server.py 

 为什么 `make server` 可以创建一个正在监听的服务器？ 

 ```
root:/mnt/d/CS2953-2024# make server
python3 server.py 25099
listening on localhost port 25099
```

我们来看看 `server.py` 里面有什么玄机。 实际上并不复杂， 我为它写了详细注释:

```python
import socket
import sys

# create a UDP socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# set the address to localhost and set the port as the first argument
addr = ('localhost', int(sys.argv[1]))
print('listening on %s port %s' % addr, file=sys.stderr)

# bind the socket to that IP address and sort
# make it enable to receive packet
sock.bind(addr)

while True:
    # receive the packet, max size is 4096
    buf, raddr = sock.recvfrom(4096)

    # decoding with utf-8
    print(buf.decode("utf-8"), file=sys.stderr)
    if buf:
        # if successfully receiving, send this message to the sender
        sent = sock.sendto(b'this is the host!', raddr)
```


## E1000 网卡

我们这里的网卡是 `E1000` 型号. 

`kernel/e1000_dev.h` 参照 E1000 的手册配置了一些宏。 E1000 的手册可以从 https://pdos.csail.mit.edu/6.828/2021/readings/8254x_GBe_SDM.pdf 访问到。 

`kernel/e1000.c` 是本次任务需要修改的主文件。 下面我对它进行一个详细介绍:
 
```c
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
  //...
  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  // ...
  // [E1000 14.4] Receive initialization
  // ...
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
```

这里的 `e1000_init()` 函数会按照手册配置 E1000 使其能进行 `DMA (Direct Memory Access)`。 `DMA` 技术能够绕过 CPU 的干预， 使网卡直接和内存进行交互， 从而极大地提高数据传输效率和系统性能。 

我们需要编写 `e1000_transmit` 和 `e1000_recv` 函数， 完成 packet 的收发。

这里还需要了解一些信息:

1.我们用于接收和发送 packet 的 `buffer` 有多个， 形成了一个 `buffer ring`. 这样可以让 packet 到达速度比 driver 处理速度更快时， 先将 packet 存入空闲的缓存。 E1000 要求每个 buffer 都以 `descriptor` 的形式提供接口。(即 `kernel/e1000_dev.h` 里的 `tx_desc` 和 `rx_desc`)

2.当网络协议栈发送 packet 时， 会调用 `e1000_transmit`， 发送一个 `mbuf` 中的 packet. 只有当 E1000 完成了发送 packet 任务， 即设置好 `descriptor` 中的 `E1000_TEXD_STAT_DD` 之后才能释放这个 `buffer`.

3.当 E1000 收到一个来自 ethernet 的 packet 之后， 先利用 `DMA` 技术把 packet 直接存入一个空闲的 `mbuf` 中， 然后引起中断。 `e1000_recv` 需要扫描整个 `RX buffer ring` ， 把 packet 用 `net_rx()` 函数送至网络协议栈进行解包。  

```c
// called by e1000 driver's interrupt handler to deliver a packet to the
// networking stack
void net_rx(struct mbuf *m)
{
  struct eth *ethhdr;
  uint16 type;

  ethhdr = mbufpullhdr(m, *ethhdr);
  if (!ethhdr) {
    mbuffree(m);
    return;
  }

  type = ntohs(ethhdr->type);
  if (type == ETHTYPE_IP)
    net_rx_ip(m);
  else if (type == ETHTYPE_ARP)
    net_rx_arp(m);
  else
    mbuffree(m);
}
```

然后我需要分配一个新的 `mbuf` 放置在此处填补空缺。

4.驱动还会通过 `memory-mapped control registers` 和 E1000 网卡进行交互。 比如， 需要通过 `E100_RDT` 查看接收的 packet 是否可用， 或者通过 `E100_TDT` 通知 E1000 驱动有一些 packet 需要发送。

## socket 相关的系统调用

`kernel/sysnet.c` 中包含了和 `socket` 相关的系统调用: `sockalloc, sockclose, sockread, sockwrite, sockrecvudp`.

我们不妨来看一下它们的实现:

### sockalloc

这个系统调用用于分配一个新的 `socket`. 这里一定要理解 `unix 中一切皆文件的思想`， 我们也是用文件的接口来处理 `socket` 的。 所以这里是分配了一个新的 `file control block`, 并设置其为 `FD_SOCK`. 另外，这里我们为每个 `socket` 都分配了一组 `buffer ring` 用于读写。实现如下, 我写了一些注释。

```c
int
sockalloc(struct file **f, uint32 raddr, uint16 lport, uint16 rport)
{
  struct sock *si, *pos;

  si = 0;
  *f = 0;
  if ((*f = filealloc()) == 0)
    goto bad;
  if ((si = (struct sock*)kalloc()) == 0)
    goto bad;

  // initialize objects
  si->raddr = raddr; // remote address
  si->lport = lport; // local port
  si->rport = rport; // remote port
  initlock(&si->lock, "sock");
  mbufq_init(&si->rxq); // initialize the mbuf queue
  (*f)->type = FD_SOCK; // set the file type to FD_SOCK
  (*f)->readable = 1;   
  (*f)->writable = 1;
  (*f)->sock = si;      // assign the socket we allocated to the file control block

  // add to list of sockets
  acquire(&lock);
  pos = sockets;
  while (pos) 
  {
    if (pos->raddr == raddr &&
        pos->lport == lport &&
	      pos->rport == rport) 
    {
      release(&lock);
      goto bad;
    }
    pos = pos->next;
  }
  si->next = sockets;
  sockets = si;
  release(&lock);
  return 0;

bad:
  if (si)
    kfree((char*)si);
  if (*f)
    fileclose(*f);
  return -1;
}
```

### sockclose

这个系统调用用于销毁一个 `socket`. 实现很简单:

``` c
void
sockclose(struct sock *si)
{
  struct sock **pos;
  struct mbuf *m;

  // remove from list of sockets
  acquire(&lock);
  pos = &sockets;
  while (*pos) 
  {
    if (*pos == si){
      *pos = si->next;
      break;
    }
    pos = &(*pos)->next;
  }
  release(&lock);

  // free any pending mbufs
  while (!mbufq_empty(&si->rxq)) 
  {
    m = mbufq_pophead(&si->rxq);
    mbuffree(m);
  }

  kfree((char*)si);
}
```

### sockread

`sockread` 系统调用即从 `socket` 中读取已经解包好的数据。 所以如何把数据放置在 `socket` 对应的 `receive buffer ring`， 以及同时做好解包， 就是我们需要编写的 `device interrupt handler` 在 E1000 发送过来一个数据包并产生中断之后需要做的事情。 

`sockread` 的实现也很简单， 如下:

```c
int
sockread(struct sock *si, uint64 addr, int n)
{
  struct proc *pr = myproc();
  struct mbuf *m;
  int len;

  acquire(&si->lock);
  while (mbufq_empty(&si->rxq) && !pr->killed) 
  {
    sleep(&si->rxq, &si->lock);
  }
  if (pr->killed) {
    release(&si->lock);
    return -1;
  }
  m = mbufq_pophead(&si->rxq);
  release(&si->lock);

  len = m->len;
  if (len > n)
    len = n;
  if (copyout(pr->pagetable, addr, m->head, len) == -1) 
  {
    mbuffree(m);
    return -1;
  }
  mbuffree(m);
  return len;
}
```

### sockwrite

`sockwrite` 系统调用是将内存中未加包的数据通过 `socket` 发送。 实现如下:

```c
int
sockwrite(struct sock *si, uint64 addr, int n)
{
  struct proc *pr = myproc();
  struct mbuf *m;

  m = mbufalloc(MBUF_DEFAULT_HEADROOM);
  // Allocate a new mbuf to hold the data

  if (!m)
    return -1;

  if (copyin(pr->pagetable, mbufput(m, n), addr, n) == -1) 
  {
    mbuffree(m);
    return -1;
  }
  net_tx_udp(m, si->raddr, si->lport, si->rport);
  // call "net_tx_udp" to pack the data and transmit the data

  return n;
}
```

这里需要注意， 把数据加包是 `sockwrite` 系统调用为我们做的， 不是程序代码里手动做的。 另外， `sockwrite` 会调用 `net_tx_udp` 进行加包， 并之后调用 `e1000_transmit` 进行发送。

# 开工！

## 测试 e1000_transmit

首先， 按照任务书指示， 我们添加一个输出测试一下:

```c
static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  // TEST_PRINT
  #ifdef TEST_PRINT
    printf("transmit!!\n");
  #endif
  // TEST_PRINT
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
```

然后一个窗口 `make server`， 另一个窗口 `make qemu $ nettests`.

结果是， 发送方显示 

```c
$ nettests
nettests running on port 25099
testing ping: transmit!!
```

而接收方没有收到包。 这很符合直觉: `nettests` 会调用 `e1000_transmit`, 所以会看到它的输出。 而我们目前并没有实现这两个函数， 所以根本看不到接收的输出。

## 实现 e1000_transmit

实际上跟着任务书的提示来写就很简单了。 

首先我需要明确， `e1000_transmit` 何时调用？ 

是用户进程需要通过某 `socket` 传输一些数据， 调用用户态接口函数， 触发系统调用 `sockwrite`; `sockwrite` 将数据打包为 `mbuf`, 然后调用 `net_tx_udp` 加 UDP 包， `net_tx_udp` 会调用 `net_tx_ip` 加 IP 包， `net_tx_ip` 调用 `net_tx_eth` 加 ethernet 包， 最后 `net_tx_eth` 调用 `e1000_transmit`, 这个函数将加包后的 `mbuf` 放入该 `socket` 对应的 `transmission buffer ring` 的空闲位置， 然后通知 E1000 设备， 由 E1000 进行真正的传输。

全部理解了之后实现就很简单了， 我在代码中写了详细的注释:

```c
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
```

## 实现 e1000_recv

接下来我们来实现 `e1000_recv`. 

同样， 我们需要知道调用栈的全部信息。 我们是如何一步步调用到 `e1000_recv` 的？

当有 `packet` 来到 E1000 设备时， E1000 会利用 `DMA` 技术将该 `packet` 直接放置到 `socket` 对应的 `receive buffer ring` 的空闲位置。 然后 E1000 会触发一个硬件中断。 CPU 会有一个核心去处理这个中断， 调用 `device(E1000) interrupt handler`. 这个 handler 是这样的:

```c
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
```

我们可以看到， 它先告诉 E1000 我们已经处理了这个中断， 之后直接调用 `e1000_recv` 去 `receive buffer ring` 中读取数据。 所以我们的函数需要做的是从正确的位置读出 `mbuf`， 调用 `net_rx` 进行自动解包， 然后分配一个新的 `mbuf` 填充 `buffer ring` 里面的空缺。 

理解之后实现就很简单了， 我写了详细的注释:

```c
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
```

# 通过测试！

接下来一个窗口 `make server`， 另一个 `make qemu $ nettests`, 可以看到通过了所有测试:


```
root/mnt/d/CS2953-2024# make server
python3 server.py 25099
listening on localhost port 25099
a message from xv6!
a message from xv6!
a message from xv6!
a message from xv6!
a message from xv6!
a message from xv6!
...

tcpdump -XXnr packets.pcap
14:10:54.752366 IP 10.0.2.15.2000 > 10.0.2.2.25099: UDP, length 19
        0x0000:  ffff ffff ffff 5254 0012 3456 0800 4500  ......RT..4V..E.
        0x0010:  002f 0000 0000 6411 3eae 0a00 020f 0a00  ./....d.>.......
        0x0020:  0202 07d0 620b 001b 0000 6120 6d65 7373  ....b.....a.mess
        0x0030:  6167 6520 6672 6f6d 2078 7636 21         age.from.xv6!
14:10:54.753041 IP 10.0.2.2.25099 > 10.0.2.15.2000: UDP, length 17
        0x0000:  5254 0012 3456 5255 0a00 0202 0800 4500  RT..4VRU......E.
        0x0010:  002d 000f 0000 4011 62a1 0a00 0202 0a00  .-....@.b.......
        0x0020:  020f 620b 07d0 0019 35fe 7468 6973 2069  ..b.....5.this.i
        0x0030:  7320 7468 6520 686f 7374 21              s.the.host!
//...
```

```
$ nettests
nettests running on port 25099
testing ping: OK
testing single-process pings: OK
testing multi-process pings: OK
testing DNS
DNS arecord for pdos.csail.mit.edu. is 128.52.129.126
DNS OK
all tests passed.
```

# 总结

这个 lab 让我学到了操作系统如何和外设交互、 如何网络收发包、 网络协议栈扮演怎样的角色。

我觉得计算机网络真的很有意思， 层层抽象封装， 都是大智慧。