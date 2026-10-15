# socket and select: a TCP server and client on this machine in one thread, UDP,
# timeouts, refused connections, addresses (Linux: the socket calls; KolibriOS: function 75)
import select
import socket

PORT = 47611

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(4)
cli = socket.create_connection(("127.0.0.1", PORT))
conn, addr = srv.accept()
print(addr[0], cli.getpeername() == ("127.0.0.1", PORT))
cli.sendall(b"hello")
print(conn.recv(100))
conn.sendall(b"world" * 3)
data = b""
while len(data) < 15:
    data += cli.recv(100)
print(data)
r, w, x = select.select([conn], [cli], [], 1.0)
print(len(r), len(w), len(x))
cli.send(b"!")
r, w, x = select.select([conn], [], [], 1.0)
print(len(r), conn.recv(10))
cli.close()
print(conn.recv(10))
conn.close()

conn2: socket.socket | None = None
with socket.socket() as c2:
    c2.connect(("localhost", PORT))
    conn2, peer = srv.accept()
    conn2.settimeout(0.3)
    print(conn2.gettimeout())
    try:
        conn2.recv(10)
    except TimeoutError as e:
        print("timeout:", e)
    except socket.timeout:
        print("socket.timeout")
conn2.close()
srv.close()

u1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u1.bind(("127.0.0.1", PORT + 1))
u2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u2.sendto(b"ping", ("127.0.0.1", PORT + 1))
data, frm = u1.recvfrom(100)
print(data)
u1.close()
u2.close()

try:
    socket.create_connection(("127.0.0.1", PORT + 2))
except ConnectionRefusedError as e:
    print("refused:", e.strerror)
print(socket.gethostbyname("localhost"), socket.gethostbyname("10.1.2.3"), socket.inet_aton("1.2.3.4"), socket.inet_ntoa(b"\x7f\x00\x00\x01"))
print(socket.htons(1), socket.ntohs(256), socket.htonl(1), socket.getaddrinfo("127.0.0.1", 80, socket.AF_INET, socket.SOCK_STREAM)[0][4])
print(socket.error is OSError, socket.timeout is TimeoutError, issubclass(socket.gaierror, OSError))
