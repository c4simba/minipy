# selectors: a listening socket and its connections watched by one selector (accept, read, echo, close),
# keys and their data, modify / unregister, get_map
import selectors
import socket
from typing import Callable

sel = selectors.DefaultSelector()
log: list[str] = []


def accept(sock: socket.socket, mask: int) -> None:
    conn, addr = sock.accept()
    log.append("accepted from " + addr[0])
    conn.setblocking(False)
    sel.register(conn, selectors.EVENT_READ, read)


def read(conn: socket.socket, mask: int) -> None:
    data = conn.recv(1000)
    if data:
        log.append("echoing " + repr(data))
        conn.sendall(data.upper())
    else:
        log.append("closing")
        sel.unregister(conn)
        conn.close()


server = socket.socket()
server.bind(("127.0.0.1", 0))
server.listen(5)
server.setblocking(False)
key = sel.register(server, selectors.EVENT_READ, accept)
print(key.events, key.fileobj is server, key.fd == server.fileno(), key.data is accept)

clients = [socket.create_connection(server.getsockname()) for i in range(2)]
for _ in range(2):
    for k, mask in sel.select(timeout=1):
        callback = k.data
        callback(k.fileobj, mask)
print(len(sel.get_map()), log)

clients[0].sendall(b"hello")
clients[1].sendall(b"world")
for _ in range(4):
    events = sel.select(timeout=0.2)
    if not events:
        break
    for k, mask in events:
        k.data(k.fileobj, mask)
print(clients[0].recv(100), clients[1].recv(100))
for c in clients:
    c.close()
for _ in range(4):
    events = sel.select(timeout=0.2)
    if not events:
        break
    for k, mask in events:
        k.data(k.fileobj, mask)
print(log, len(sel.get_map()))

k2 = sel.modify(server, selectors.EVENT_READ | selectors.EVENT_WRITE, accept)
print(k2.events, sel.get_key(server).events, server in sel.get_map())
try:
    sel.register(server, selectors.EVENT_READ, accept)
except KeyError as e:
    print("KeyError: already registered")
try:
    sel.register(server, 0, accept)
except ValueError as e:
    print("ValueError:", e)
sel.unregister(server)
print(server in sel.get_map(), sel.select(timeout=0))
sel.close()
server.close()
