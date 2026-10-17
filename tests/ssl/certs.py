# The peer's certificate as CPython's ssl gives it (the interpreter: tests/run_ssl_tests.sh)
import socket
import ssl
import sys

ctx = ssl.create_default_context(cafile="tests/ssl/cert.pem")
with ctx.wrap_socket(socket.create_connection(("127.0.0.1", int(sys.argv[1]))), server_hostname="localhost") as s:
    cert = s.getpeercert()
    for k in sorted(cert):
        print(k, cert[k])
    der = s.getpeercert(True)
    print("der", len(der), der[:4].hex())
