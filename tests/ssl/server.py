"""A TLS server for tests/run_ssl_tests.sh, run by CPython: tests/ssl/cert.pem (localhost,
127.0.0.1). An HTTP request gets a page naming its path; other data comes back upper-cased.
Prints its port, then serves until killed."""
import os
import socket
import ssl
import sys
import threading

here = os.path.dirname(os.path.abspath(__file__))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(os.path.join(here, "cert.pem"), os.path.join(here, "key.pem"))
if len(sys.argv) > 1 and sys.argv[1] == "--client-cert":
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(os.path.join(here, "cert.pem"))

listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(16)
print(listener.getsockname()[1], flush=True)


def serve(raw):
    try:
        with ctx.wrap_socket(raw, server_side=True) as s:
            data = s.recv(65536)
            if data.startswith(b"GET ") or data.startswith(b"POST "):
                while b"\r\n\r\n" not in data:
                    more = s.recv(65536)
                    if not more:
                        break
                    data += more
                path = data.split(b" ")[1].decode()
                body = ("you asked for " + path + "\n").encode()
                s.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + str(len(body)).encode()
                          + b"\r\nConnection: close\r\n\r\n" + body)
            else:
                while data:
                    s.sendall(data.upper())
                    data = s.recv(65536)
    except (ssl.SSLError, OSError):
        pass


while True:
    conn, _ = listener.accept()
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
