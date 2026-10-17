# A TLS server in plainpy (interpreter and compiled): one connection, its data back upper-cased;
# tests/ssl/talk.py (CPython) is the client (tests/run_ssl_tests.sh)
import socket
import ssl
import sys

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("tests/ssl/cert.pem", "tests/ssl/key.pem")
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(1)
print(listener.getsockname()[1])
sys.stdout.flush()
tls = ctx.wrap_socket(listener, server_side=True)
conn, addr = tls.accept()
data = conn.recv(1000)
while data:
    conn.sendall(data.upper())
    data = conn.recv(1000)
conn.close()
tls.close()
