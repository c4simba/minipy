# http.server: a handler of its own (GET, POST, errors, 501), SimpleHTTPRequestHandler (files, listing, redirect,
# 404); the client in the same thread: its request waits in the socket until handle_request() serves it
import http.client
import http.server
import io
import os
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path == "/missing":
            self.send_error(404)
            return
        body = ("GET " + self.path + " ua=" + str(self.headers.get("User-Agent")) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        n = int(self.headers.get("Content-Length") or "0")
        data = self.rfile.read(n)
        self.send_response(http.HTTPStatus.CREATED, "Made")
        self.send_header("X-Len", str(len(data)))
        self.end_headers()
        self.wfile.write(data.upper())

    def log_message(self, format: str, *args: sys._PercentArgs) -> None:
        pass


class Files(http.server.SimpleHTTPRequestHandler):
    def log_date_time_string(self) -> str:
        return "DATE"


def show(r: http.client.HTTPResponse) -> None:
    print(r.status, r.reason, r.getheader("Content-Type"), r.getheader("X-Len"), r.read(), flush=True)


server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
for method, path, body in (("GET", "/hello?x=1", None), ("POST", "/data", b"some data"), ("GET", "/missing", None),
                           ("DELETE", "/x", None)):
    conn = http.client.HTTPConnection("127.0.0.1", port)
    if body is None:
        conn.request(method, path, headers={"User-Agent": "test/1.0"})
    else:
        conn.request(method, path, body=body)
    server.handle_request()
    r = conn.getresponse()
    if r.status >= 400:
        page = r.read().decode()
        print(r.status, r.reason, r.getheader("Content-Type"), [p.split("</p>")[0] for p in page.split("<p>")[1:]],
              flush=True)
    else:
        show(r)
    conn.close()
server.server_close()

# the files of a directory
root = "build/typed_httpd"
os.makedirs(root + "/sub", exist_ok=True)
with open(root + "/a.txt", "w") as f:
    f.write("text file\n")
with open(root + "/sub/index.html", "w") as f:
    f.write("<p>index</p>\n")
os.chdir(root)
files = http.server.HTTPServer(("127.0.0.1", 0), Files)
port = files.server_address[1]
log = io.StringIO()
for path in ("/a.txt", "/sub/", "/sub", "/", "/nothing.txt"):
    conn = http.client.HTTPConnection("127.0.0.1", port)
    conn.request("GET", path)
    err = sys.stderr
    files.handle_request()
    r = conn.getresponse()
    body = r.read()
    if path == "/":
        print(r.status, [line for line in body.decode().split("\n") if line.startswith("<li>")], flush=True)
    else:
        print(r.status, r.reason, r.getheader("Content-Type"), r.getheader("Content-Length"), r.getheader("Location"),
              repr(body) if r.status == 200 else str(len(body)), flush=True)
    conn.close()
files.server_close()
os.chdir("../..")
for name in ("/a.txt", "/sub/index.html", "/sub", ""):
    if name.endswith(".txt") or name.endswith(".html"):
        os.remove(root + name)
    else:
        os.rmdir(root + name)
print(http.server.BaseHTTPRequestHandler.responses[404], http.server.SimpleHTTPRequestHandler.server_version, flush=True)
