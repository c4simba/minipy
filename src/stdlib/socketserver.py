"""Generic socket server classes (CPython's socketserver): TCPServer, UDPServer, ThreadingTCPServer,
ThreadingUDPServer, ThreadingMixIn; BaseRequestHandler, StreamRequestHandler, DatagramRequestHandler.

There are no forking or Unix-domain servers (no fork(), no AF_UNIX here). Compiled programs: a
handler's request is the connection's socket (UDP: the server's socket, the datagram in
self.packet - a BaseRequestHandler there finds it in self.server.packet), its server a BaseServer."""
import sys
import socket
import select
import threading
from io import BytesIO
from time import monotonic as time

__all__ = ["BaseServer", "TCPServer", "UDPServer", "ThreadingUDPServer", "ThreadingTCPServer",
           "BaseRequestHandler", "StreamRequestHandler", "DatagramRequestHandler", "ThreadingMixIn"]


class BaseServer:
    """Base class for server classes."""

    timeout: float | None = None

    def __init__(self, server_address: tuple[str, int], RequestHandlerClass: "type[BaseRequestHandler]") -> None:
        """Constructor.  May be extended, do not override."""
        self.server_address = server_address
        self.RequestHandlerClass = RequestHandlerClass
        self._is_shut_down = threading.Event()
        self._shutdown_request = False
        self.packet = b""                   # (UDP: the datagram being handled)

    def fileno(self) -> int:
        return -1

    def server_activate(self) -> None:
        """Called by constructor to activate the server."""
        pass

    def _ready(self, timeout: float | None) -> bool:
        return len(select.select([self], [], [], timeout)[0]) > 0

    def serve_forever(self, poll_interval: float = 0.5) -> None:
        """Handle one request at a time until shutdown.

        Polls for shutdown every poll_interval seconds."""
        self._is_shut_down.clear()
        try:
            while not self._shutdown_request:
                ready = self._ready(poll_interval)
                if self._shutdown_request:
                    break
                if ready:
                    self._handle_request_noblock()
                self.service_actions()
        finally:
            self._shutdown_request = False
            self._is_shut_down.set()

    def shutdown(self) -> None:
        """Stops the serve_forever loop (call it from another thread); waits until it has finished."""
        self._shutdown_request = True
        self._is_shut_down.wait()

    def service_actions(self) -> None:
        """Called by the serve_forever() loop."""
        pass

    def handle_request(self) -> None:
        """Handle one request, possibly blocking.  Respects self.timeout."""
        timeout = self._socket_timeout()
        if timeout is None:
            timeout = self.timeout
        elif self.timeout is not None:
            timeout = min(timeout, self.timeout)
        deadline = 0.0
        if timeout is not None:
            deadline = time() + timeout
        while True:
            if self._ready(timeout):
                self._handle_request_noblock()
                return
            if timeout is not None:
                timeout = deadline - time()
                if timeout < 0:
                    self.handle_timeout()
                    return

    def _socket_timeout(self) -> float | None:
        return None

    def _handle_request_noblock(self) -> None:
        try:
            request, client_address = self.get_request()
        except OSError:
            return
        if self.verify_request(request, client_address):
            try:
                self.process_request(request, client_address)
            except Exception:
                self.handle_error(request, client_address)
                self.shutdown_request(request)
            except BaseException:
                self.shutdown_request(request)
                raise
        else:
            self.shutdown_request(request)

    def get_request(self) -> tuple[socket.socket, tuple[str, int]]:
        raise NotImplementedError

    def handle_timeout(self) -> None:
        """Called if no new request arrives within self.timeout."""
        pass

    def verify_request(self, request: socket.socket, client_address: tuple[str, int]) -> bool:
        """Verify the request.  May be overridden.  Return True if we should proceed with this request."""
        return True

    def process_request(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        """Call finish_request.  Overridden by ThreadingMixIn."""
        self.finish_request(request, client_address)
        self.shutdown_request(request)

    def server_close(self) -> None:
        """Called to clean-up the server."""
        pass

    def finish_request(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        """Finish one request by instantiating RequestHandlerClass."""
        self.RequestHandlerClass(request, client_address, self)

    def shutdown_request(self, request: socket.socket) -> None:
        """Called to shutdown and close an individual request."""
        self.close_request(request)

    def close_request(self, request: socket.socket) -> None:
        """Called to clean up an individual request."""
        pass

    def handle_error(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        """Handle an error gracefully: print a traceback and continue."""
        print('-' * 40, file=sys.stderr)
        print('Exception occurred during processing of request from', client_address, file=sys.stderr)
        import traceback
        traceback.print_exc()
        print('-' * 40, file=sys.stderr)

    def __enter__(self) -> "BaseServer":
        return self

    def __exit__(self, et, ev, tb) -> None:
        self.server_close()


class TCPServer(BaseServer):
    """Base class for various socket-based server classes (TCP by default)."""

    address_family = socket.AF_INET
    socket_type = socket.SOCK_STREAM
    request_queue_size = 5
    allow_reuse_address = False
    allow_reuse_port = False

    def __init__(self, server_address: tuple[str, int], RequestHandlerClass: "type[BaseRequestHandler]",
                 bind_and_activate: bool = True) -> None:
        """Constructor.  May be extended, do not override."""
        BaseServer.__init__(self, server_address, RequestHandlerClass)
        self.socket = socket.socket(self.address_family, self.socket_type)
        if bind_and_activate:
            try:
                self.server_bind()
                self.server_activate()
            except BaseException:
                self.server_close()
                raise

    def server_bind(self) -> None:
        """Called by constructor to bind the socket."""
        if self.allow_reuse_address:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if self.allow_reuse_port:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.socket.bind(self.server_address)
        self.server_address = self.socket.getsockname()

    def server_activate(self) -> None:
        """Called by constructor to activate the server."""
        self.socket.listen(self.request_queue_size)

    def server_close(self) -> None:
        """Called to clean-up the server."""
        self.socket.close()

    def fileno(self) -> int:
        """Return socket file number (for select)."""
        return self.socket.fileno()

    def _socket_timeout(self) -> float | None:
        return self.socket.gettimeout()

    def get_request(self) -> tuple[socket.socket, tuple[str, int]]:
        """Get the request and client address from the socket."""
        return self.socket.accept()

    def shutdown_request(self, request: socket.socket) -> None:
        """Called to shutdown and close an individual request."""
        try:
            request.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        self.close_request(request)

    def close_request(self, request: socket.socket) -> None:
        """Called to clean up an individual request."""
        request.close()


class UDPServer(TCPServer):
    """UDP server class."""

    allow_reuse_address = False
    allow_reuse_port = False
    socket_type = socket.SOCK_DGRAM
    max_packet_size = 8192

    if not sys._compiled:
        def get_request(self):
            data, client_addr = self.socket.recvfrom(self.max_packet_size)
            return (data, self.socket), client_addr

    if sys._compiled:
        def get_request(self) -> tuple[socket.socket, tuple[str, int]]:
            data, client_addr = self.socket.recvfrom(self.max_packet_size)
            self.packet = data
            return self.socket, client_addr

    def server_activate(self) -> None:
        pass

    def shutdown_request(self, request: socket.socket) -> None:
        self.close_request(request)

    def close_request(self, request: socket.socket) -> None:
        pass


class ThreadingMixIn:
    """Mix-in class to handle each request in a new thread."""

    daemon_threads = False
    block_on_close = True

    def process_request_thread(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        """Same as in BaseServer but as a thread.  In addition, exception handling is done here."""
        try:
            self.finish_request(request, client_address)
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.shutdown_request(request)

    def process_request(self, request: socket.socket, client_address: tuple[str, int]) -> None:
        """Start a new thread to process the request."""
        t = threading.Thread(target=self.process_request_thread, args=(request, client_address))
        t.daemon = self.daemon_threads
        if self.block_on_close and not t.daemon:
            self._thread_list().append(t)
        t.start()

    _threads_: list[threading.Thread] | None = None

    def _thread_list(self) -> list[threading.Thread]:
        threads = self._threads_
        if threads is None:
            threads = []
            self._threads_ = threads
        return threads

    def server_close(self) -> None:
        super().server_close()
        if self.block_on_close:
            threads = self._thread_list()
            while threads:
                threads.pop(0).join()


class ThreadingUDPServer(ThreadingMixIn, UDPServer):
    pass


class ThreadingTCPServer(ThreadingMixIn, TCPServer):
    pass


class BaseRequestHandler:
    """Base class for request handler classes: one is made for each request; its handle() serves it
    (self.request, self.client_address, self.server)."""

    def __init__(self, request: socket.socket, client_address: tuple[str, int], server: BaseServer) -> None:
        self.request = request
        self.client_address = client_address
        self.server = server
        self.setup()
        try:
            self.handle()
        finally:
            self.finish()

    def setup(self) -> None:
        pass

    def handle(self) -> None:
        pass

    def finish(self) -> None:
        pass


class _SocketWriter:
    """Simple writable file to a socket: data is sent at once (no flush() needed)."""

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self.closed = False

    def writable(self) -> bool:
        return True

    def write(self, b: bytes) -> int:
        self._sock.sendall(b)
        return len(b)

    def fileno(self) -> int:
        return self._sock.fileno()

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True


class StreamRequestHandler(BaseRequestHandler):
    """Define self.rfile and self.wfile for stream sockets."""

    rbufsize = -1
    wbufsize = 0
    timeout: float | None = None
    disable_nagle_algorithm = False

    def setup(self) -> None:
        self.connection = self.request
        if self.timeout is not None:
            self.connection.settimeout(self.timeout)
        if self.disable_nagle_algorithm:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.rfile = self.connection.makefile('rb', self.rbufsize)
        self.wfile = _SocketWriter(self.connection)

    def finish(self) -> None:
        if not self.wfile.closed:
            try:
                self.wfile.flush()
            except OSError:
                pass
        self.wfile.close()
        self.rfile.close()


class DatagramRequestHandler(BaseRequestHandler):
    """Define self.rfile and self.wfile for datagram sockets."""

    if not sys._compiled:
        def setup(self):
            self.packet, self.socket = self.request
            self.rfile = BytesIO(self.packet)
            self.wfile = BytesIO()

    if sys._compiled:
        def setup(self) -> None:
            self.packet = self.server.packet
            self.socket = self.request
            self.rfile = BytesIO(self.packet)
            self.wfile = BytesIO()

    def finish(self) -> None:
        self.socket.sendto(self.wfile.getvalue(), self.client_address)
