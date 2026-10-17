# The peer's certificate in a compiled program: CPython's keys, the values as text
# (tests/ssl/fields.out; tests/run_ssl_tests.sh)
import socket
import ssl
import sys

ctx = ssl.create_default_context(cafile="tests/ssl/cert.pem")
s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", int(sys.argv[1]))), server_hostname="localhost")
cert = s.getpeercert()
if cert is not None:
    for k in sorted(cert):
        if k != "serialNumber":
            print(k, cert[k])
der = s.getpeercert_der()
if der is not None:
    print("der", len(der), der[:4].hex())
s.close()
