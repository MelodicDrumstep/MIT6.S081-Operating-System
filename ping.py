import socket
import sys
import time

# create a udp socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# set the address as localhost, port as the first argument passed by command line
addr = ('localhost', int(sys.argv[1]))
buf = "this is a ping!".encode('utf-8')

while True:
	print("pinging...", file=sys.stderr)
	# send a ping message to localhost
	sock.sendto(buf, ("127.0.0.1", int(sys.argv[1])))
	time.sleep(1)
