"""CPython's ssl client for tests/ssl/serve.py: argv[1] the port."""
import socket
import ssl
import sys

ctx = ssl.create_default_context(cafile="tests/ssl/cert.pem")
with ctx.wrap_socket(socket.create_connection(("127.0.0.1", int(sys.argv[1]))), server_hostname="localhost") as s:
    print("server:", s.version(), s.getpeercert()["subject"][-1])
    for word in (b"one", b"two words", b"three"):
        s.sendall(word)
        print("got", s.recv(100))
