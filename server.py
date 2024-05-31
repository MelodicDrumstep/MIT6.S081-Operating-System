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
