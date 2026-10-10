# socketserver: TCP servers with stream and base handlers (handle_request: the client's data waits in the
# socket); socket.makefile() in binary and text mode
import socket
import socketserver


class Echo(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        line = self.rfile.readline()
        self.wfile.write(b"echo: " + line.upper())
        print("handled", self.client_address[0], repr(line))


class Upper(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        data = self.request.recv(1024)
        self.request.sendall(data.upper())


with socketserver.TCPServer(("127.0.0.1", 0), Echo) as server:
    host, port = server.server_address
    print(host, port > 0)
    for word in (b"hello\n", b"second line\nrest"):
        c = socket.create_connection(("127.0.0.1", port))
        c.sendall(word)
        server.handle_request()
        print(c.recv(1024))
        c.close()

srv = socketserver.TCPServer(("127.0.0.1", 0), Upper)
c = socket.create_connection(srv.server_address)
c.sendall(b"abc")
srv.handle_request()
print(c.recv(100))
c.close()
srv.server_close()

# makefile: text and binary (over a TCP connection: what is written waits in the socket)
listener = socket.socket()
listener.bind(("127.0.0.1", 0))
listener.listen(1)
a = socket.create_connection(listener.getsockname())
b, peer = listener.accept()
w = a.makefile("w")
w.write("line one\nline two\n")
w.flush()
wb = a.makefile("wb")
wb.write(b"bytes\r\n")
w.close()
wb.close()
a.close()
rb = b.makefile("rb")
print(rb.readline(), rb.readline(), rb.readline(), rb.read())
rb.close()
b.close()
a2 = socket.create_connection(listener.getsockname())
b2, peer = listener.accept()
a2.sendall("t\u00e9xt\r\nend".encode())
a2.close()
r = b2.makefile("r", encoding="utf-8")
print(repr(r.readline()), repr(r.read()))
r.close()
b2.close()
listener.close()
