"""urllib.parse (split, join, quote, queries) and http.client against a server in the same program (loopback;
KolibriOS: function 75 sockets)."""
from urllib.parse import urlparse, urlsplit, urlunparse, urlunsplit, urljoin, urldefrag, quote, quote_plus, unquote, unquote_plus, urlencode, parse_qs, parse_qsl, quote_from_bytes, unquote_to_bytes
import urllib.parse

for u in ["http://user:pw@Example.COM:8080/a/b;p?q=1&r=2#frag", "https://[::1]:443/x", "ftp://host/file.txt", "mailto:joe@example.com",
          "/rel/path?x", "//host/only", "file:///etc/hosts", "http://h/p;a;b", "  http://h/sp ace\t\n", "kolibri:/rd/1/x", "http://h:/x", "x:y"]:
    sr = urlsplit(u)
    pr = urlparse(u)
    print(repr(u), sr, pr.params, sr.username, sr.password, sr.hostname, sr.port, sr.geturl(), pr.geturl())
print(urlsplit("x", scheme="http"), urlsplit("a#b", allow_fragments=False))
for bad in ["http://[::1/x", "http://h:99999/", "http://h:abc/"]:
    try:
        print(urlsplit(bad).port)
    except ValueError as e:
        print("ValueError", e)
print(urlunsplit(("http", "h", "/p", "q=1", "f")), urlunparse(("http", "h", "/p", "x", "q", "")), urlunsplit(("", "", "//p", "", "")), urlunsplit(("http", "", "p", "", "")))
base = "http://a/b/c/d;p?q"
for rel in ["g:h", "g", "./g", "g/", "/g", "//g", "?y", "g?y", "#s", "g#s", "g?y#s", ";x", "g;x", "", ".", "./", "..", "../", "../g", "../..", "../../", "../../g", "../../../g", "/./g", "/../g", "g.", ".g", "g..", "..g", "./../g", "./g/.", "g/./h", "g/../h", "http:g"]:
    print(repr(rel), urljoin(base, rel))
print(urljoin("http://a/b", "c"), urljoin("http://a/b/", "c"), urljoin("", "x"), urljoin("http://a", ""), urljoin("ftp://a/b", "http://c/d"))
print(urldefrag("http://a/b#c"), urldefrag("http://a/b"), urldefrag("http://a/b#c").geturl())
for s in ["abc def", "a/b?c=d&e", "żółw", "~user", "", "100%"]:
    print(repr(s), quote(s), quote(s, safe=""), quote_plus(s), quote_plus(s, safe="/"), unquote(quote(s)) == s)
print(unquote("a%20b%2Fc%zz%4"), unquote("%C5%BC%C3%B3%C5%82w"), unquote("%E2%82"), unquote_plus("a+b%2Bc"), unquote("ąę%20x"), unquote_to_bytes("a%00%ff"), quote_from_bytes(b"\x00\xffab/"))
print(parse_qsl("a=1&b=2&a=3&c=&d"), parse_qsl("a=1&b=&c", keep_blank_values=True), parse_qs("a=1&b=2&a=3"), parse_qs("x=a+b%20c"), parse_qsl("a=1;b=2", separator=";"))
try:
    parse_qsl("a&b=1", strict_parsing=True)
except ValueError as e:
    print("ValueError", e)
print(urlencode({"a": "1", "b": "x y", "c": "ż&"}), urlencode({"n": 1, "m": 22}), urlencode([("k", "v1"), ("k", "v2")]), urlencode({"q": "a/b"}, safe="/"), urlencode({"q": "a b"}, quote_via=quote))
print(urlencode({"k": ["1", "2"]}, doseq=True), urllib.parse.quote("x y"))
sp = urlsplit("http://h/p?q#f")
print(sp[0], sp[-1], len(sp), list(sp), sp._replace(path="/new").geturl(), sp == urlsplit("http://h/p?q#f"))

import socket
import http.client
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError
from http.client import HTTPConnection, responses

PORT = 47623
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(5)


def serve(reply: bytes) -> bytes:
    c, a = srv.accept()
    data = b""
    while b"\r\n\r\n" not in data:
        data += c.recv(65536)
    head, sep, rest = data.partition(b"\r\n\r\n")
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1].strip())
            while len(rest) < n:
                rest += c.recv(65536)
    c.sendall(reply)
    c.close()
    return head + sep + rest


conn = HTTPConnection("127.0.0.1", PORT, timeout=5)
conn.request("GET", "/index.html?q=1", headers={"X-Test": "yes"})
req = serve(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 11\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\nhello world")
print(req)
r = conn.getresponse()
print(r.status, r.reason, r.version, r.getheader("content-length"), r.getheader("set-cookie"), r.getheader("nope", "dflt"))
print(r.headers.get_content_type(), r.headers.get_content_charset(), r.getheaders(), len(r.headers), "Content-Type" in r.headers)
print(r.read(5), r.read(), r.read(), r.isclosed())
conn.close()

conn = HTTPConnection("127.0.0.1:" + str(PORT))
conn.request("POST", "/form", body="a=1&b=2", headers={"Content-Type": "application/x-www-form-urlencoded"})
req = serve(b"HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n4\r\nWiki\r\n6;ext=1\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\nTrailer: x\r\n\r\n")
print(req)
r = conn.getresponse()
print(r.status, r.reason, r.chunked, r.will_close, r.read(), r.isclosed())
conn.close()

conn = HTTPConnection("127.0.0.1", PORT)
conn.request("HEAD", "/h")
serve(b"HTTP/1.0 404 Not Found\r\nContent-Length: 99\r\n\r\n")
r = conn.getresponse()
print(r.status, r.reason, r.version, r.read(), r.will_close)
conn.close()

conn = HTTPConnection("127.0.0.1", PORT)
conn.request("PUT", "/p", body="\x00\x01\xe9")
print(serve(b"HTTP/1.1 204 No Content\r\n\r\n"))
r = conn.getresponse()
print(r.status, r.read(), r.length)
conn.close()

conn = HTTPConnection("127.0.0.1", PORT)
conn.request("GET", "/close")
serve(b"HTTP/1.0 200 OK\r\n\r\nuntil the end")
r = conn.getresponse()
print(r.read(), r.will_close)
conn.close()

conn = HTTPConnection("127.0.0.1", PORT)
conn.request("GET", "/bad")
serve(b"garbage\r\n\r\n")
try:
    conn.getresponse()
except http.client.BadStatusLine as e:
    print("BadStatusLine", e)
conn.close()

conn = HTTPConnection("127.0.0.1", PORT)
conn.request("GET", "/none")
serve(b"")
try:
    conn.getresponse()
except http.client.RemoteDisconnected as e:
    print("RemoteDisconnected", e)
conn.close()

conn = HTTPConnection("127.0.0.1", PORT)
try:
    conn.getresponse()
except http.client.ResponseNotReady as e:
    print("ResponseNotReady", e)
try:
    HTTPConnection("host:abc")
except http.client.InvalidURL as e:
    print("InvalidURL", e)
c2 = HTTPConnection("example.com")
print(c2.host, c2.port, HTTPConnection("[::1]:8080").host, responses[404], responses[200], http.client.HTTP_PORT)
srv.close()


rq = Request("http://example.com:8080/a/b?x=1#frag", headers={"x-custom": "v", "Accept": "*/*"})
print(rq.full_url, rq.type, rq.host, rq.selector, rq.get_method(), rq.fragment, sorted(rq.header_items()), rq.has_header("X-custom"), rq.get_header("Accept"))
r2 = Request("http://h/", data=b"a=1", method=None)
print(r2.get_method(), r2.selector, r2.data)
r2.add_header("content-type", "text/plain")
r2.full_url = "http://other/x"
print(r2.host, r2.selector, r2.get_header("Content-type"), Request("http://h", method="PUT").get_method())
try:
    Request("no-scheme")
except ValueError as e:
    print("ValueError", e)
try:
    urlopen("http://127.0.0.1:47699/", timeout=2)
except URLError as e:
    print("URLError", type(e).__name__, isinstance(e, OSError))
h = HTTPError("http://h/x", 404, "Not Found", None, None) if False else None
print(h is None)
