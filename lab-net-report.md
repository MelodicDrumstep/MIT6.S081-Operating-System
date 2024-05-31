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
 
