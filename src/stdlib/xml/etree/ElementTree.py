"""Lightweight XML support (CPython's xml.etree.ElementTree): Element, SubElement, ElementTree, parse,
fromstring / XML, tostring, indent, dump, iselement, register_namespace, ParseError; an XML parser of
its own (no expat): elements, attributes, text, CDATA, character and the five predefined entities,
namespaces ({uri}tag); comments, processing instructions and the DOCTYPE are skipped.

find / findall / findtext / iterfind take ElementPath's paths: tag, *, ., .., //, {uri}tag,
[@attrib], [@attrib='value'], [tag], [tag='text'], [n], [last()], [last()-n].

Compiled programs: the tags of Comment() / ProcessingInstruction() elements are strings ("\\0!comment",
"\\0?pi"), not those functions; tostring(e, encoding="unicode") is a str, other encodings bytes."""
import sys
from typing import TypeVar

__all__ = ["Comment", "dump", "Element", "ElementTree", "fromstring", "fromstringlist", "indent", "iselement",
           "parse", "ParseError", "PI", "ProcessingInstruction", "register_namespace", "SubElement", "tostring",
           "tostringlist", "XML", "XMLID", "VERSION"]

VERSION = "1.3.0"

_F = TypeVar("_F")
_P = TypeVar("_P")



class ParseError(SyntaxError):
    """An error while parsing XML: .position is (line, column), .code the expat error code."""

    def __init__(self, msg: str, code: int = 0, position: tuple[int, int] = (0, 0)) -> None:
        super().__init__(msg)
        self.code = code
        self.position = position


class Element:
    """An XML element: tag, attrib (a dict), text, tail and its subelements (a sequence)."""

    def __init__(self, tag: str, attrib: dict[str, str] | None = None, **extra: str) -> None:
        self.tag = tag
        self.attrib: dict[str, str] = {}
        if attrib is not None:
            for k in attrib:
                self.attrib[k] = attrib[k]
        for k in extra:
            self.attrib[k] = extra[k]
        self.text: str | None = None
        self.tail: str | None = None
        self._children: list[Element] = []

    def __repr__(self) -> str:
        return "<%s %r at %#x>" % (self.__class__.__name__, self.tag, id(self))

    def makeelement(self, tag: str, attrib: dict[str, str]) -> "Element":
        """Create a new element with the same type."""
        return Element(tag, attrib)

    def copy(self) -> "Element":
        elem = self.makeelement(self.tag, self.attrib)
        elem.text = self.text
        elem.tail = self.tail
        elem._children = list(self._children)
        return elem

    def __copy__(self) -> "Element":
        return self.copy()

    def __len__(self) -> int:
        return len(self._children)

    def __bool__(self) -> bool:
        return len(self._children) != 0

    def __getitem__(self, index: int) -> "Element":
        return self._children[index]

    def __setitem__(self, index: int, element: "Element") -> None:
        self._children[index] = element

    def __delitem__(self, index: int) -> None:
        del self._children[index]

    def __iter__(self):
        for e in list(self._children):
            yield e

    def append(self, subelement: "Element") -> None:
        """Add *subelement* to the end of this element."""
        self._children.append(subelement)

    def extend(self, elements: list["Element"]) -> None:
        """Append subelements from a sequence."""
        for e in elements:
            self._children.append(e)

    def insert(self, index: int, subelement: "Element") -> None:
        """Insert *subelement* at position *index*."""
        self._children.insert(index, subelement)

    def remove(self, subelement: "Element") -> None:
        """Remove matching subelement (the same object)."""
        for i in range(len(self._children)):
            if self._children[i] is subelement:
                del self._children[i]
                return
        raise ValueError("list.remove(x): x not in list")

    def find(self, path: str, namespaces: dict[str, str] | None = None) -> "Element | None":
        """Find first matching element by tag name or path (None: no element matches)."""
        for e in _select(self, path, namespaces):
            return e
        return None

    def findtext(self, path: str, default: str | None = None, namespaces: dict[str, str] | None = None) -> str | None:
        """Find text for first matching element by tag name or path ("" when it has no text)."""
        for e in _select(self, path, namespaces):
            return e.text or ""
        return default

    def findall(self, path: str, namespaces: dict[str, str] | None = None) -> list["Element"]:
        """Find all matching subelements by tag name or path."""
        return _select(self, path, namespaces)

    def iterfind(self, path: str, namespaces: dict[str, str] | None = None):
        """Find all matching subelements by tag name or path: an iterator."""
        for e in _select(self, path, namespaces):
            yield e

    def clear(self) -> None:
        """Reset element: its subelements, attributes, text and tail."""
        self.attrib = {}
        self._children = []
        self.text = None
        self.tail = None

    def get(self, key: str, default: str | None = None) -> str | None:
        """Get element attribute (default when it has none of that name)."""
        return self.attrib.get(key, default)

    def set(self, key: str, value: str) -> None:
        """Set element attribute."""
        self.attrib[key] = value

    def keys(self) -> list[str]:
        """Get list of attribute names (in the order they were set)."""
        return list(self.attrib.keys())

    def items(self) -> list[tuple[str, str]]:
        """Get element attributes as a sequence of (name, value) pairs."""
        return list(self.attrib.items())

    def iter(self, tag: str | None = None):
        """Create tree iterator: this element and all elements below it, in document order (tag: those
        of that tag; "*": all)."""
        if tag == "*":
            tag = None
        for e in self._iter_list(tag, []):
            yield e

    def _iter_list(self, tag: str | None, out: list["Element"]) -> list["Element"]:
        if tag is None or self.tag == tag:
            out.append(self)
        for e in self._children:
            e._iter_list(tag, out)
        return out

    def itertext(self):
        """Create text iterator: the texts of this element and its subelements, in document order."""
        for t in self._texts([]):
            yield t

    def _texts(self, out: list[str]) -> list[str]:
        if self.tag == _COMMENT_TAG or self.tag == _PI_TAG:
            return out
        t = self.text
        if t:
            out.append(t)
        for e in self._children:
            e._texts(out)
            tail = e.tail
            if tail:
                out.append(tail)
        return out


def iselement(element: _F) -> bool:
    """Return True if *element* appears to be an Element."""
    return isinstance(element, Element)


def SubElement(parent: Element, tag: str, attrib: dict[str, str] | None = None, **extra: str) -> Element:
    """Subelement factory: an element appended to parent."""
    element = parent.makeelement(tag, attrib or {})
    for k in extra:
        element.attrib[k] = extra[k]
    parent.append(element)
    return element


def Comment(text: str | None = None) -> Element:
    """Comment element factory."""
    element = Element(_COMMENT_TAG)
    element.text = text
    return element


def ProcessingInstruction(target: str, text: str | None = None) -> Element:
    """Processing Instruction element factory."""
    element = Element(_PI_TAG)
    element.text = target
    if text:
        element.text = target + " " + text
    return element


# ---- paths (a subset of ElementPath)

def _resolve_ns(name: str, namespaces: dict[str, str] | None) -> str:
    if name[:1] == "{" or ":" not in name or namespaces is None:
        if namespaces is not None and name[:1] != "{" and name not in ("*", ".", "..") and "" in namespaces and \
                not name.startswith("@"):
            return "{%s}%s" % (namespaces[""], name)
        return name
    prefix, local = name.split(":", 1)
    if prefix not in namespaces:
        raise SyntaxError("prefix %r not found in prefix map" % prefix)
    return "{%s}%s" % (namespaces[prefix], local)


def _tokens(path: str) -> list[str]:
    """The steps of a path, with their predicates: ["tag", "[@a='v']", "/", "//", ...]"""
    out: list[str] = []
    i = 0
    n = len(path)
    while i < n:
        c = path[i]
        if c == "/":
            if path.startswith("//", i):
                out.append("//")
                i += 2
            else:
                out.append("/")
                i += 1
        elif c == "[":
            j = i + 1
            quote = ""
            while j < n:
                if quote:
                    if path[j] == quote:
                        quote = ""
                elif path[j] in "'\"":
                    quote = path[j]
                elif path[j] == "]":
                    break
                j += 1
            if j >= n:
                raise SyntaxError("expected ']'")
            out.append(path[i:j + 1])
            i = j + 1
        elif c == "{":
            j = path.find("}", i)
            if j < 0:
                raise SyntaxError("expected '}'")
            k = j + 1
            while k < n and path[k] not in "/[":
                k += 1
            out.append(path[i:k])
            i = k
        else:
            k = i
            while k < n and path[k] not in "/[":
                k += 1
            out.append(path[i:k].strip())
            i = k
    return out


def _children_of(elems: list[Element], tag: str) -> list[Element]:
    out: list[Element] = []
    for e in elems:
        for c in e._children:
            if c.tag == _COMMENT_TAG or c.tag == _PI_TAG:
                continue
            if tag == "*" or c.tag == tag or (tag.startswith("{*}") and c.tag.split("}")[-1] == tag[3:]) or \
                    (tag.endswith("}*") and c.tag.startswith(tag[:-1])):
                out.append(c)
    return out


def _descendants_of(elems: list[Element], tag: str) -> list[Element]:
    out: list[Element] = []
    seen: set[int] = set()
    for e in elems:
        for d in e._iter_list(None, []):
            if d is e or d.tag == _COMMENT_TAG or d.tag == _PI_TAG:
                continue
            if tag == "*" or d.tag == tag or (tag.startswith("{*}") and d.tag.split("}")[-1] == tag[3:]):
                if id(d) not in seen:
                    seen.add(id(d))
                    out.append(d)
    return out


def _parent_map(root: Element) -> dict[int, Element]:
    m: dict[int, Element] = {}
    for p in root._iter_list(None, []):
        for c in p._children:
            m[id(c)] = p
    return m


def _unquote(s: str) -> str:
    s = s.strip()
    if len(s) >= 2 and s[0] in "'\"" and s[-1] == s[0]:
        return s[1:-1]
    return s


def _apply_predicate(elems: list[Element], pred: str, namespaces: dict[str, str] | None) -> list[Element]:
    p = pred[1:-1].strip()
    if p.startswith("@"):
        if "=" in p:
            name, value = p[1:].split("=", 1)
            name = _resolve_ns(name.strip(), namespaces)
            neg = name.endswith("!")
            if neg:
                name = name[:-1].strip()
            v = _unquote(value)
            return [e for e in elems if (e.get(name) != v if neg else e.get(name) == v)]
        name = _resolve_ns(p[1:].strip(), namespaces)
        return [e for e in elems if e.get(name) is not None]
    if p.isdigit() or p.startswith("last()") or p.startswith("-"):
        if p.isdigit():
            index = int(p) - 1
            if index < 0:
                raise SyntaxError("XPath position >= 1 expected")
        elif p == "last()":
            index = -1
        elif p.startswith("last()-"):
            index = -1 - int(p[7:])
        else:
            raise SyntaxError("unsupported expression")
        return [elems[index]] if -len(elems) <= index < len(elems) else []
    if "=" in p:
        name, value = p.split("=", 1)
        name = name.strip()
        v = _unquote(value)
        neg = name.endswith("!")
        if neg:
            name = name[:-1].strip()
        if name == ".":
            return [e for e in elems if ("".join(e.itertext()) != v if neg else "".join(e.itertext()) == v)]
        tag = _resolve_ns(name, namespaces)
        out: list[Element] = []
        for e in elems:
            hit = False
            for c in e._children:
                if (tag == "*" or c.tag == tag) and "".join(c.itertext()) == v:
                    hit = True
            if hit != neg:
                out.append(e)
        return out
    tag = _resolve_ns(p, namespaces)
    return [e for e in elems if len(_children_of([e], tag)) > 0]


def _select(elem: Element, path: str, namespaces: dict[str, str] | None) -> list[Element]:
    toks = _tokens(path)
    if toks and toks[0] == "/":
        raise SyntaxError("cannot use absolute path on element")
    cur: list[Element] = [elem]
    i = 0
    parents: dict[int, Element] | None = None
    while i < len(toks):
        t = toks[i]
        if t == "/":
            i += 1
            continue
        if t == "//":
            i += 1
            if i >= len(toks):
                raise SyntaxError("invalid path")
            tag = _resolve_ns(toks[i], namespaces)
            cur = _descendants_of(cur, tag)
            i += 1
        elif t == ".":
            i += 1
        elif t == "..":
            if parents is None:
                parents = _parent_map(elem)
            up: list[Element] = []
            for e in cur:
                pe = parents.get(id(e))
                if pe is not None and pe not in up:
                    up.append(pe)
            cur = up
            i += 1
        elif t.startswith("["):
            raise SyntaxError("invalid predicate")
        else:
            cur = _children_of(cur, _resolve_ns(t, namespaces))
            i += 1
        # predicates of the step: per parent for positions, as ElementPath
        while i < len(toks) and toks[i].startswith("["):
            pred = toks[i]
            inner = pred[1:-1].strip()
            if inner.isdigit() or inner.startswith("last()"):
                if parents is None:
                    parents = _parent_map(elem)
                groups: dict[int, list[Element]] = {}
                order: list[int] = []
                for e in cur:
                    pe = parents.get(id(e))
                    key = id(pe) if pe is not None else 0
                    if key not in groups:
                        groups[key] = []
                        order.append(key)
                    groups[key].append(e)
                picked: list[Element] = []
                for key in order:
                    picked.extend(_apply_predicate(groups[key], pred, namespaces))
                cur = picked
            else:
                cur = _apply_predicate(cur, pred, namespaces)
            i += 1
    return cur


# ---- serializing

def _escape_cdata(text: str) -> str:
    if "&" in text:
        text = text.replace("&", "&amp;")
    if "<" in text:
        text = text.replace("<", "&lt;")
    if ">" in text:
        text = text.replace(">", "&gt;")
    return text


def _escape_attrib(text: str) -> str:
    if "&" in text:
        text = text.replace("&", "&amp;")
    if "<" in text:
        text = text.replace("<", "&lt;")
    if ">" in text:
        text = text.replace(">", "&gt;")
    if "\"" in text:
        text = text.replace("\"", "&quot;")
    if "\r" in text:
        text = text.replace("\r", "&#13;")
    if "\n" in text:
        text = text.replace("\n", "&#10;")
    if "\t" in text:
        text = text.replace("\t", "&#09;")
    return text


_namespace_map: dict[str, str] = {
    # "well-known" namespace prefixes
    "http://www.w3.org/XML/1998/namespace": "xml",
    "http://www.w3.org/1999/xhtml": "html",
    "http://www.w3.org/1999/02/22-rdf-syntax-ns#": "rdf",
    "http://schemas.xmlsoap.org/wsdl/": "wsdl",
    # xml schema
    "http://www.w3.org/2001/XMLSchema": "xs",
    "http://www.w3.org/2001/XMLSchema-instance": "xsi",
    # dublin core
    "http://purl.org/dc/elements/1.1/": "dc",
}


def register_namespace(prefix: str, uri: str) -> None:
    """Register a namespace prefix (used when serializing)."""
    if prefix[:2] == "ns" and prefix[2:].isdigit():
        raise ValueError("Prefix format reserved for internal use")
    for k in list(_namespace_map.keys()):
        if k == uri or _namespace_map[k] == prefix:
            del _namespace_map[k]
    _namespace_map[uri] = prefix


def _namespaces(elem: Element, default_namespace: str | None) -> tuple[dict[str, str], dict[str, str]]:
    qnames: dict[str, str] = {}
    namespaces: dict[str, str] = {}
    if default_namespace:
        namespaces[default_namespace] = ""

    def add_qname(qname: str) -> None:
        if qname[:1] == "{":
            uri, tag = qname[1:].rsplit("}", 1)
            prefix = namespaces.get(uri)
            if prefix is None:
                prefix = _namespace_map.get(uri)
                if prefix is None:
                    prefix = "ns%d" % len(namespaces)
                if prefix != "xml":
                    namespaces[uri] = prefix
            if prefix:
                qnames[qname] = "%s:%s" % (prefix, tag)
            else:
                qnames[qname] = tag  # default element
        else:
            if default_namespace:
                raise ValueError("cannot use non-qualified names with default_namespace option")
            qnames[qname] = qname

    for e in elem.iter():
        tag = e.tag
        if tag != _COMMENT_TAG and tag != _PI_TAG and tag not in qnames:
            add_qname(tag)
        for key in e.attrib:
            if key not in qnames:
                add_qname(key)
    return qnames, namespaces


def _serialize_xml(out: list[str], elem: Element, qnames: dict[str, str], namespaces: dict[str, str] | None,
                   short_empty_elements: bool) -> None:
    tag = elem.tag
    text = elem.text
    if tag == _COMMENT_TAG:
        out.append("<!--%s-->" % (text or ""))
    elif tag == _PI_TAG:
        out.append("<?%s?>" % (text or ""))
    else:
        qtag = qnames[tag]
        out.append("<" + qtag)
        items = list(elem.attrib.items())
        if namespaces:
            for v, k in sorted(namespaces.items(), key=lambda x: x[1]):  # sort on prefix
                if k:
                    k = ":" + k
                out.append(" xmlns%s=\"%s\"" % (k, _escape_attrib(v)))
        for k, v in items:
            out.append(" %s=\"%s\"" % (qnames[k], _escape_attrib(v)))
        if text or len(elem) or not short_empty_elements:
            out.append(">")
            if text:
                out.append(_escape_cdata(text))
            for e in elem._children:
                _serialize_xml(out, e, qnames, None, short_empty_elements)
            out.append("</" + qtag + ">")
        else:
            out.append(" />")
    tail = elem.tail
    if tail:
        out.append(_escape_cdata(tail))


def _serialize_text(out: list[str], elem: Element) -> None:
    for part in elem.itertext():
        out.append(part)
    if elem.tail:
        out.append(elem.tail)


def _as_text(element: Element, method: str | None, xml_declaration: bool | None, default_namespace: str | None,
             short_empty_elements: bool, declared: str) -> str:
    m = method or "xml"
    if m not in ("xml", "text", "html"):
        raise ValueError("unknown method %r" % m)
    out: list[str] = []
    if m == "xml" and (xml_declaration or (xml_declaration is None and declared.lower() != "unicode" and
                                           declared.lower() not in ("utf-8", "us-ascii"))):
        out.append("<?xml version='1.0' encoding='%s'?>\n" % (declared if declared.lower() != "unicode" else "UTF-8",))
    if m == "text":
        _serialize_text(out, element)
    else:
        qnames, namespaces = _namespaces(element, default_namespace)
        _serialize_xml(out, element, qnames, namespaces, short_empty_elements)
    return "".join(out)


def _encode(text: str, encoding: str) -> bytes:
    if encoding.lower() in ("us-ascii", "ascii"):
        return text.encode("ascii", "xmlcharrefreplace")
    return text.encode(encoding, "xmlcharrefreplace")


if not sys._compiled:
    def tostring(element, encoding=None, method=None, *, xml_declaration=None, default_namespace=None,
                 short_empty_elements=True):
        """Generate string representation of XML element: bytes ("unicode" encoding: a str)."""
        if encoding == "unicode":
            return _tostring_unicode(element, encoding, method, xml_declaration=xml_declaration,
                                     default_namespace=default_namespace, short_empty_elements=short_empty_elements)
        return _tostring_bytes(element, encoding, method, xml_declaration=xml_declaration,
                               default_namespace=default_namespace, short_empty_elements=short_empty_elements)

    def tostringlist(element, encoding=None, method=None, *, xml_declaration=None, default_namespace=None,
                     short_empty_elements=True):
        return [tostring(element, encoding, method, xml_declaration=xml_declaration,
                         default_namespace=default_namespace, short_empty_elements=short_empty_elements)]


if sys._compiled:
    def tostring(element: Element, encoding: str | None = None, method: str | None = None, *,
                 xml_declaration: bool | None = None, default_namespace: str | None = None,
                 short_empty_elements: bool = True) -> bytes:
        """Generate string representation of XML element: bytes (encoding="unicode": a str - the compiler
        calls _tostring_unicode for that literal)."""
        return _tostring_bytes(element, encoding, method, xml_declaration=xml_declaration,
                               default_namespace=default_namespace, short_empty_elements=short_empty_elements)

    def _tostringlist_unicode(element: Element, encoding: str | None = None, method: str | None = None, *,
                              xml_declaration: bool | None = None, default_namespace: str | None = None,
                              short_empty_elements: bool = True) -> list[str]:
        return [_tostring_unicode(element, encoding, method, xml_declaration=xml_declaration,
                                  default_namespace=default_namespace, short_empty_elements=short_empty_elements)]

    def tostringlist(element: Element, encoding: str | None = None, method: str | None = None, *,
                     xml_declaration: bool | None = None, default_namespace: str | None = None,
                     short_empty_elements: bool = True) -> list[bytes]:
        return [_tostring_bytes(element, encoding, method, xml_declaration=xml_declaration,
                                default_namespace=default_namespace, short_empty_elements=short_empty_elements)]


def _tostring_unicode(element: Element, encoding: str | None = None, method: str | None = None, *,
                      xml_declaration: bool | None = None, default_namespace: str | None = None,
                      short_empty_elements: bool = True) -> str:
    return _as_text(element, method, xml_declaration, default_namespace, short_empty_elements, "unicode")


def _tostring_bytes(element: Element, encoding: str | None = None, method: str | None = None, *,
                    xml_declaration: bool | None = None, default_namespace: str | None = None,
                    short_empty_elements: bool = True) -> bytes:
    enc = encoding or "us-ascii"
    return _encode(_as_text(element, method, xml_declaration, default_namespace, short_empty_elements, enc), enc)


def dump(elem: Element) -> None:
    """Write element tree or element structure to sys.stdout (for debugging)."""
    text = _tostring_unicode(elem)
    sys.stdout.write(text)
    tail = elem.tail
    if not tail or tail[-1] != "\n":
        sys.stdout.write("\n")


def indent(tree: _F, space: str = "  ", level: int = 0) -> None:
    """Indent an XML document by inserting newlines and indentation space after elements."""
    if isinstance(tree, ElementTree):
        root = tree.getroot()
    else:
        root = tree
    if level < 0:
        raise ValueError(f"Initial indentation level must be >= 0, got {level}")
    if not len(root):
        return
    indentations = ["\n" + level * space]
    _indent_children(root, 0, indentations, space)


def _indent_children(elem: Element, level: int, indentations: list[str], space: str) -> None:
    # Start a new indentation level for the first child.
    child_level = level + 1
    if child_level < len(indentations):
        child_indentation = indentations[child_level]
    else:
        child_indentation = indentations[level] + space
        indentations.append(child_indentation)
    t = elem.text
    if not t or not t.strip():
        elem.text = child_indentation
    last: Element | None = None
    for child in elem._children:
        if len(child):
            _indent_children(child, child_level, indentations, space)
        tail = child.tail
        if not tail or not tail.strip():
            child.tail = child_indentation
        last = child
    # Dedent after the last child by overwriting the previous indentation.
    if last is not None:
        tail = last.tail
        if tail is None or not tail.strip():
            last.tail = indentations[level]


# ---- parsing

_ENTITIES = {"lt": "<", "gt": ">", "amp": "&", "quot": "\"", "apos": "'"}


class _Parser:
    """A small non-validating XML parser making Elements."""

    def __init__(self, text: str) -> None:
        self.s = text.replace("\r\n", "\n").replace("\r", "\n")
        self.i = 0
        self.n = len(self.s)
        self.ns_stack: list[dict[str, str]] = [{"xml": "http://www.w3.org/XML/1998/namespace"}]
        self.ids: dict[str, Element] = {}

    def position(self, at: int) -> tuple[int, int]:
        line = self.s.count("\n", 0, at) + 1
        col = at - (self.s.rfind("\n", 0, at) + 1)
        return (line, col)

    def error(self, what: str, code: int, at: int) -> ParseError:
        line, col = self.position(at)
        return ParseError("%s: line %d, column %d" % (what, line, col), code, (line, col))

    def skip_misc(self) -> None:
        """Whitespace, comments, processing instructions, the XML declaration and the DOCTYPE."""
        while self.i < self.n:
            c = self.s[self.i]
            if c in " \t\n":
                self.i += 1
            elif self.s.startswith("<!--", self.i):
                j = self.s.find("-->", self.i + 4)
                if j < 0:
                    raise self.error("unclosed token", 5, self.i)
                self.i = j + 3
            elif self.s.startswith("<?", self.i):
                j = self.s.find("?>", self.i + 2)
                if j < 0:
                    raise self.error("unclosed token", 5, self.i)
                self.i = j + 2
            elif self.s.startswith("<!DOCTYPE", self.i):
                depth = 0
                j = self.i
                while j < self.n:
                    if self.s[j] == "[":
                        depth += 1
                    elif self.s[j] == "]":
                        depth -= 1
                    elif self.s[j] == ">" and depth <= 0:
                        break
                    j += 1
                if j >= self.n:
                    raise self.error("unclosed token", 5, self.i)
                self.i = j + 1
            else:
                return

    def name(self) -> str:
        start = self.i
        while self.i < self.n and (self.s[self.i].isalnum() or self.s[self.i] in "_:-." or ord(self.s[self.i]) > 127):
            self.i += 1
        if self.i == start:
            raise self.error("not well-formed (invalid token)", 4, self.i)
        return self.s[start:self.i]

    def unescape(self, text: str, at: int) -> str:
        if "&" not in text:
            return text
        out: list[str] = []
        i = 0
        while True:
            j = text.find("&", i)
            if j < 0:
                out.append(text[i:])
                return "".join(out)
            out.append(text[i:j])
            k = text.find(";", j)
            if k < 0:
                raise self.error("not well-formed (invalid token)", 4, at + j)
            ent = text[j + 1:k]
            if ent.startswith("#x"):
                out.append(chr(int(ent[2:], 16)))
            elif ent.startswith("#"):
                out.append(chr(int(ent[1:])))
            elif ent in _ENTITIES:
                out.append(_ENTITIES[ent])
            else:
                raise self.error("undefined entity", 11, at + j)
            i = k + 1

    def qualify(self, name: str, attr: bool) -> str:
        ns = self.ns_stack[-1]
        if ":" in name:
            prefix, local = name.split(":", 1)
            if prefix not in ns:
                raise self.error("unbound prefix", 27, self.i)
            return "{%s}%s" % (ns[prefix], local)
        if not attr and "" in ns and ns[""]:
            return "{%s}%s" % (ns[""], name)
        return name

    def element(self) -> Element:
        start = self.i
        self.i += 1                                   # <
        raw = self.name()
        attrs: list[tuple[str, str]] = []
        ns = dict(self.ns_stack[-1])
        while True:
            ws = self.i
            while self.i < self.n and self.s[self.i] in " \t\n":
                self.i += 1
            if self.i >= self.n:
                raise self.error("unclosed token", 5, start)
            c = self.s[self.i]
            if c == ">" or self.s.startswith("/>", self.i):
                break
            if ws == self.i:
                raise self.error("not well-formed (invalid token)", 4, self.i)
            astart = self.i
            an = self.name()
            while self.i < self.n and self.s[self.i] in " \t\n":
                self.i += 1
            if self.i >= self.n or self.s[self.i] != "=":
                raise self.error("not well-formed (invalid token)", 4, self.i)
            self.i += 1
            while self.i < self.n and self.s[self.i] in " \t\n":
                self.i += 1
            if self.i >= self.n or self.s[self.i] not in "'\"":
                raise self.error("not well-formed (invalid token)", 4, self.i)
            q = self.s[self.i]
            j = self.s.find(q, self.i + 1)
            if j < 0:
                raise self.error("unclosed token", 5, start)
            raw_value = self.s[self.i + 1:j]
            if "<" in raw_value:
                raise self.error("not well-formed (invalid token)", 4, self.i + 1 + raw_value.index("<"))
            value = self.unescape(raw_value.replace("\n", " ").replace("\t", " "), self.i + 1)
            self.i = j + 1
            if an == "xmlns":
                ns[""] = value
            elif an.startswith("xmlns:"):
                ns[an[6:]] = value
            else:
                for k, v in attrs:
                    if k == an:
                        raise self.error("duplicate attribute", 8, astart)
                attrs.append((an, value))
        self.ns_stack.append(ns)
        elem = Element(self.qualify(raw, False))
        for k, v in attrs:
            elem.attrib[self.qualify(k, True)] = v
        if self.s.startswith("/>", self.i):
            self.i += 2
            self.ns_stack.pop()
            return elem
        self.i += 1                                   # >
        last: Element | None = None
        text: list[str] = []
        while True:
            if self.i >= self.n:
                raise self.error("no element found", 3, self.n)
            if self.s.startswith("</", self.i):
                self.flush(elem, last, text)
                at = self.i
                self.i += 2
                end = self.name()
                while self.i < self.n and self.s[self.i] in " \t\n":
                    self.i += 1
                if end != raw:
                    raise self.error("mismatched tag", 7, at + 2)
                if self.i >= self.n or self.s[self.i] != ">":
                    raise self.error("not well-formed (invalid token)", 4, self.i)
                self.i += 1
                self.ns_stack.pop()
                return elem
            if self.s.startswith("<!--", self.i):
                j = self.s.find("-->", self.i + 4)
                if j < 0:
                    raise self.error("unclosed token", 5, self.i)
                self.i = j + 3
            elif self.s.startswith("<![CDATA[", self.i):
                j = self.s.find("]]>", self.i + 9)
                if j < 0:
                    raise self.error("unclosed CDATA section", 6, self.i)
                text.append(self.s[self.i + 9:j])
                self.i = j + 3
            elif self.s.startswith("<?", self.i):
                j = self.s.find("?>", self.i + 2)
                if j < 0:
                    raise self.error("unclosed token", 5, self.i)
                self.i = j + 2
            elif self.s[self.i] == "<":
                self.flush(elem, last, text)
                child = self.element()
                elem.append(child)
                last = child
            else:
                j = self.s.find("<", self.i)
                if j < 0:
                    j = self.n
                chunk = self.s[self.i:j]
                if "]]>" in chunk:
                    raise self.error("not well-formed (invalid token)", 4, self.i + chunk.index("]]>"))
                text.append(self.unescape(chunk, self.i))
                self.i = j

    def flush(self, elem: Element, last: Element | None, text: list[str]) -> None:
        if not text:
            return
        t = "".join(text)
        text.clear()
        if last is None:
            elem.text = (elem.text or "") + t
        else:
            last.tail = (last.tail or "") + t

    def parse(self) -> Element:
        self.skip_misc()
        if self.i >= self.n:
            raise self.error("no element found", 3, self.n)
        if self.s[self.i] != "<":
            raise self.error("syntax error", 2, self.i)
        root = self.element()
        self.skip_misc()
        if self.i < self.n:
            raise self.error("junk after document element", 9, self.i)
        return root


def XML(text: _F, parser: _P = None) -> Element:
    """Parse XML document from string constant (str or bytes): its root element."""
    if isinstance(text, bytes):
        return _Parser(_decode(text)).parse()
    return _Parser(text).parse()


def _decode(data: bytes) -> str:
    enc = "utf-8"
    if data.startswith(b"<?xml"):
        end = data.find(b"?>")
        decl = data[:end].decode("ascii", "replace")
        k = decl.find("encoding=")
        if k >= 0:
            q = decl[k + 9:k + 10]
            enc = decl[k + 10:decl.find(q, k + 10)]
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.decode(enc)


fromstring = XML


def fromstringlist(sequence: list[str], parser: _P = None) -> Element:
    """Parse XML document from sequence of string fragments."""
    return _Parser("".join(sequence)).parse()


def XMLID(text: str, parser: _P = None) -> tuple[Element, dict[str, Element]]:
    """Parse XML document from string constant: (root, a dict of the elements with an id attribute by it)."""
    root = XML(text)
    ids: dict[str, Element] = {}
    for elem in root.iter():
        i = elem.get("id")
        if i is not None:
            ids[i] = elem
    return root, ids


class ElementTree:
    """An XML element hierarchy: its root element (made by parse() from a file)."""

    def __init__(self, element: Element | None = None, file: _F = None) -> None:
        self._root = element
        if file is not None:
            self.parse(file)

    def getroot(self) -> Element:
        """Return root element of this tree."""
        root = self._root
        if root is None:
            raise TypeError("ElementTree not initialized")
        return root

    def _setroot(self, element: Element) -> None:
        self._root = element

    def parse(self, source: _F, parser: _P = None) -> Element:
        """Load external XML document into element tree (a file name or a file object)."""
        if isinstance(source, str):
            with open(source, "rb") as f:
                data = f.read()
        else:
            data = source.read()
        if isinstance(data, str):
            root = _Parser(data).parse()
        else:
            root = _Parser(_decode(data)).parse()
        self._root = root
        return root

    def iter(self, tag: str | None = None):
        """Create and return tree iterator for the root element."""
        return self.getroot().iter(tag)

    def find(self, path: str, namespaces: dict[str, str] | None = None) -> Element | None:
        """Find first matching element by tag name or path."""
        if path[:1] == "/":
            path = "." + path
        return self.getroot().find(path, namespaces)

    def findtext(self, path: str, default: str | None = None, namespaces: dict[str, str] | None = None) -> str | None:
        """Find first matching element by tag name or path: its text."""
        if path[:1] == "/":
            path = "." + path
        return self.getroot().findtext(path, default, namespaces)

    def findall(self, path: str, namespaces: dict[str, str] | None = None) -> list[Element]:
        """Find all matching subelements by tag name or path."""
        if path[:1] == "/":
            path = "." + path
        return self.getroot().findall(path, namespaces)

    def iterfind(self, path: str, namespaces: dict[str, str] | None = None):
        """Find all matching subelements by tag name or path: an iterator."""
        if path[:1] == "/":
            path = "." + path
        return self.getroot().iterfind(path, namespaces)

    def write(self, file_or_filename: _F, encoding: str | None = None, xml_declaration: bool | None = None,
              default_namespace: str | None = None, method: str | None = None, *,
              short_empty_elements: bool = True) -> None:
        """Write element tree to a file (a name or a file object) as XML."""
        enc = encoding or "us-ascii"
        text = _as_text(self.getroot(), method, xml_declaration, default_namespace, short_empty_elements, enc)
        if isinstance(file_or_filename, str):
            with open(file_or_filename, "wb") as f:
                f.write(_encode(text, enc if enc.lower() != "unicode" else "utf-8"))
        elif enc.lower() == "unicode":
            file_or_filename.write(text)
        else:
            file_or_filename.write(_encode(text, enc))


def parse(source: _F, parser: _P = None) -> ElementTree:
    """Parse XML document into element tree (source: a file name or a file object)."""
    tree = ElementTree()
    tree.parse(source)
    return tree


PI = ProcessingInstruction

if sys._compiled:
    _COMMENT_TAG = "\0!comment"             # (comment and processing instruction elements: their tags)
    _PI_TAG = "\0?pi"
else:
    _COMMENT_TAG = Comment                  # (CPython's: the factories)
    _PI_TAG = ProcessingInstruction
