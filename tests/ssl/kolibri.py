# ssl on KolibriOS (plainpy's over mbedtls.obj), compiled and run there by tests/run_kolibri_ssl_tests.sh:
# a client of tests/ssl/server.py (CPython, on the host: 10.0.2.2 in QEMU's user network), then a
# server tests/ssl/talk.py (CPython) talks to through QEMU's port forwarding. What it finds goes to
# /hd0/1/kout.txt (the test's folder), compared with tests/ssl/kolibri.out.
import http.client
import socket
import ssl
import sys

HERE = "/hd0/1/"


def log(s: str) -> None:
    f = open(HERE + "kout.txt", "a")
    f.write(s + "\n")
    f.close()


f = open(HERE + "port.txt", "r")
port = int(f.read().strip())
f.close()
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.load_verify_locations(HERE + "cert.pem")
log("context: " + str(ctx.check_hostname) + " " + str(ctx.verify_mode == ssl.CERT_REQUIRED))
sys.syscall(5, 2000)                         # (20 s for DHCP, started at boot: a connection before it can hang)
raw = socket.socket()
for attempt in range(30):
    try:
        raw = socket.create_connection(("10.0.2.2", port))
        break
    except OSError:
        sys.syscall(5, 100)
s = ctx.wrap_socket(raw, server_hostname="localhost")
log("version: " + str(s.version()))
c = s.cipher()
log("cipher: " + (c[0] + " " + str(c[2]) if c is not None else "none"))
s.sendall(b"hello over tls")
log("echo: " + str(s.recv(100)))
cert = s.getpeercert()
if cert is not None:
    for k in sorted(cert):
        if k != "serialNumber":
            log(k + ": " + cert[k])
der = s.getpeercert_der()
if der is not None:
    log("der: " + str(len(der)) + " bytes, " + der[:4].hex())
s.close()

try:
    raw = socket.create_connection(("10.0.2.2", port))
    ssl.create_default_context().wrap_socket(raw, server_hostname="localhost")
    log("no CAs: accepted")
except ssl.SSLCertVerificationError as e:
    log("no CAs: " + str(e.reason) + " - " + e.verify_message)

try:
    raw = socket.create_connection(("10.0.2.2", port))
    ctx.wrap_socket(raw, server_hostname="example.com")
    log("another host: accepted")
except ssl.SSLCertVerificationError as e:
    log("another host: " + str(e.reason) + " - " + e.verify_message)

hctx = ssl.create_default_context(cafile=HERE + "cert.pem")
hctx.check_hostname = False
conn = http.client.HTTPSConnection("10.0.2.2", port, context=hctx)
conn.request("GET", "/from/kolibrios")
r = conn.getresponse()
log("http.client: " + str(r.status) + " " + r.reason + " " + str(r.read()))
conn.close()

server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
server.load_cert_chain(HERE + "cert.pem", HERE + "key.pem")
listener = socket.socket()
listener.bind(("0.0.0.0", 8443))
listener.listen(1)
log("listening")
tls = server.wrap_socket(listener, server_side=True)
conn2, addr = tls.accept()
log("accepted: " + str(conn2.version()))
data = conn2.recv(1000)
while data:
    conn2.sendall(data.upper())
    data = conn2.recv(1000)
conn2.close()
log("done")
