# FastAPI for MiniPy: the routing part of FastAPI's API, for compiled programs
# (minipy --compile) and the interpreter. Serve an app with uvicorn.run(app).
#
#   from fastapi import FastAPI
#
#   app = FastAPI()
#
#   @app.get("/items/{item_id}")
#   def read_item(item_id: int, q: str | None = None):
#       return {"item_id": item_id, "q": q}
#
# Path parameters ({name} in the path) and query parameters (the other
# parameters) are int, float, bool or str, with defaults; the result is
# written as JSON (dicts, lists, numbers, strings, objects: their fields).
# Errors answer as FastAPI does: 404 / 405, 422 with the parameter's details,
# HTTPException(status_code, detail). Not here: request bodies (pydantic
# models), dependencies, response models, OpenAPI (/docs).
#
# How a function becomes a route: `fn: Endpoint` - passing a function where a
# minipy.Endpoint is expected gives an adapter the compiler writes for it
# (parameters from a dict of strings, the result as JSON).
import json
import sys
from typing import Callable, Optional
from minipy import Endpoint, endpoint

class HTTPException(Exception):
    def __init__(self, status_code: int, detail: str = ""):
        super().__init__(detail)
        self.status_code = status_code
        self.detail = detail

class Response:
    def __init__(self, status_code: int, body: str, media_type: str = "application/json"):
        self.status_code = status_code
        self.body = body
        self.media_type = media_type

class Route:
    def __init__(self, path: str, methods: list[str], handler: Callable[[dict[str, str]], str], name: str):
        self.path = path
        self.methods = methods
        self.handler = handler
        self.name = name
        self.parts = split_path(path)

    def match(self, parts: list[str], params: dict[str, str]) -> bool:
        """Does the path match? Its {parameters} go into params."""
        if len(parts) != len(self.parts):
            return False
        found: dict[str, str] = {}
        for i in range(len(parts)):
            mine = self.parts[i]
            if mine.startswith("{") and mine.endswith("}"):
                found[mine[1:len(mine) - 1]] = unquote(parts[i], False)
            elif mine != parts[i]:
                return False
        for k in found:
            params[k] = found[k]
        return True

def split_path(path: str) -> list[str]:
    parts: list[str] = []
    for p in path.split("/"):
        if p != "":
            parts.append(p)
    return parts

def hex_value(ch: str) -> int:
    digits = "0123456789abcdef"
    return digits.find(ch.lower())

def unquote(text: str, plus: bool) -> str:
    """%XX escapes (and + for a space in a query string)."""
    if text.find("%") < 0 and (not plus or text.find("+") < 0):
        return text
    out: list[str] = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "%" and i + 2 < len(text) and hex_value(text[i + 1]) >= 0 and hex_value(text[i + 2]) >= 0:
            out.append(chr(16 * hex_value(text[i + 1]) + hex_value(text[i + 2])))
            i += 3
            continue
        if ch == "+" and plus:
            out.append(" ")
        else:
            out.append(ch)
        i += 1
    return utf8("".join(out))

def utf8(text: str) -> str:
    """chr() of bytes >= 0x80 gave characters: back to the bytes they were."""
    for ch in text:
        if ord(ch) >= 128:
            data: list[int] = []
            for c in text:
                data.append(ord(c) & 255)
            return bytes_text(data)
    return text

def bytes_text(data: list[int]) -> str:
    b = sys.buffer(len(data))
    for i in range(len(data)):
        sys.poke(b, i, data[i], 1)
    return sys.peek_str(b, 0, len(data))

def parse_query(query: str) -> dict[str, str]:
    params: dict[str, str] = {}
    if query == "":
        return params
    for pair in query.split("&"):
        if pair == "":
            continue
        eq = pair.find("=")
        if eq < 0:
            params[unquote(pair, True)] = ""
        else:
            params[unquote(pair[:eq], True)] = unquote(pair[eq + 1:], True)
    return params

def error_body(kind: str, where: str, name: str, typ: str, given: Optional[str]) -> str:
    """FastAPI's 422 answer for one parameter."""
    msg = "Field required"
    t = "missing"
    if kind == "parsing":
        t = typ + "_parsing"
        if typ == "int":
            msg = "Input should be a valid integer, unable to parse string as an integer"
        elif typ == "float":
            msg = "Input should be a valid number, unable to parse string as a number"
        else:
            msg = "Input should be a valid boolean, unable to interpret input"
    detail = {"type": t, "loc": [where, name], "msg": msg, "input": given}
    return json.dumps({"detail": [detail]}, separators=(",", ":"), ensure_ascii=False)

def detail_body(detail: str) -> str:
    return json.dumps({"detail": detail}, separators=(",", ":"), ensure_ascii=False)

class FastAPI:
    def __init__(self, title: str = "FastAPI", version: str = "0.1.0"):
        self.title = title
        self.version = version
        self.routes: list[Route] = []

    def add_api_route(self, path: str, fn: Endpoint, methods: list[str], name: str = "") -> None:
        self.routes.append(Route(path, methods, endpoint(fn), name))

    def api_route(self, path: str, methods: list[str]) -> Callable[[Endpoint], Endpoint]:
        def decorator(fn: Endpoint) -> Endpoint:
            self.add_api_route(path, fn, methods)
            return fn
        return decorator

    def get(self, path: str) -> Callable[[Endpoint], Endpoint]:
        return self.api_route(path, ["GET"])

    def post(self, path: str) -> Callable[[Endpoint], Endpoint]:
        return self.api_route(path, ["POST"])

    def put(self, path: str) -> Callable[[Endpoint], Endpoint]:
        return self.api_route(path, ["PUT"])

    def delete(self, path: str) -> Callable[[Endpoint], Endpoint]:
        return self.api_route(path, ["DELETE"])

    def patch(self, path: str) -> Callable[[Endpoint], Endpoint]:
        return self.api_route(path, ["PATCH"])

    def handle(self, method: str, target: str) -> Response:
        """The answer to `method target` (target: path?query)."""
        path = target
        query = ""
        q = target.find("?")
        if q >= 0:
            path = target[:q]
            query = target[q + 1:]
        parts = split_path(path)
        wrong_method = False
        for route in self.routes:
            params = parse_query(query)
            if not route.match(parts, params):
                continue
            if method not in route.methods:
                wrong_method = True
                continue
            try:
                return Response(200, route.handler(params))
            except HTTPException as e:
                return Response(e.status_code, detail_body(e.detail))
            except ValueError as e:
                text = str(e)
                if not text.startswith("#endpoint\n"):
                    raise
                f = text.split("\n")
                where = "query"
                if ("{" + f[2] + "}") in route.parts:
                    where = "path"
                given: Optional[str] = None
                if f[1] == "parsing":
                    given = "\n".join(f[4:])
                return Response(422, error_body(f[1], where, f[2], f[3], given))
        if wrong_method:
            return Response(405, detail_body("Method Not Allowed"))
        return Response(404, detail_body("Not Found"))
