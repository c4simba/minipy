# TLS through ssl (the system's OpenSSL 3), against tests/ssl/server.py: the same output from
# CPython, plainpy and compiled programs (tests/run_ssl_tests.sh).
import sys
import socket
import ssl
import http.client
import urllib.request

port = int(sys.argv[1])
cafile = "tests/ssl/cert.pem"

ctx = ssl.create_default_context(cafile=cafile)
print("context:", ctx.check_hostname, ctx.verify_mode == ssl.CERT_REQUIRED)
raw = socket.create_connection(("127.0.0.1", port))
s = ctx.wrap_socket(raw, server_hostname="localhost")
print("version:", s.version())
c = s.cipher()
print("cipher protocol:", c[1] if c is not None else None)
s.sendall(b"hello over tls")
print("echo:", s.recv(100))
cert = s.getpeercert()
print("notAfter:", cert["notAfter"])
print("notBefore:", cert["notBefore"])
s.close()

try:
    raw = socket.create_connection(("127.0.0.1", port))
    ssl.create_default_context().wrap_socket(raw, server_hostname="localhost")
    print("system CAs: accepted?")
except ssl.SSLCertVerificationError as e:
    print("system CAs:", e.reason, "-", e.verify_message)

try:
    raw = socket.create_connection(("127.0.0.1", port))
    ctx.wrap_socket(raw, server_hostname="example.com")
    print("other host: accepted?")
except ssl.SSLCertVerificationError as e:
    print("other host:", e.reason, "-", e.verify_message)

u = ssl._create_unverified_context()
raw = socket.create_connection(("127.0.0.1", port))
s = u.wrap_socket(raw)
s.sendall(b"unchecked")
print("unverified:", s.recv(100), s.getpeercert())
s.close()

conn = http.client.HTTPSConnection("localhost", port, context=ctx)
conn.request("GET", "/over/https")
r = conn.getresponse()
print("http.client:", r.status, r.reason, r.read())
conn.close()

with urllib.request.urlopen("https://localhost:" + str(port) + "/urllib", context=ctx) as page:
    print("urlopen:", page.status, page.read())

sc = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
try:
    sc.load_cert_chain("tests/ssl/missing.pem")
except (OSError, ssl.SSLError) as e:
    print("missing cert:", type(e).__name__)
