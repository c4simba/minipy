"""Shell-like lexical analysis (CPython's shlex): split, quote, join and the shlex lexer.

The lexer reads a str (in the interpreter also a file object, read whole); source
inclusion (the source attribute) is not supported."""
import sys
from typing import TypeVar

_P = TypeVar("_P")

__all__ = ["shlex", "split", "quote", "join"]


class shlex:
    """A lexical analyzer for shell-like syntaxes: get_token() (None or '' at the end), iteration."""

    def __init__(self, instream: str | None = None, infile: str | None = None, posix: bool = False,
                 punctuation_chars: _P = False):
        text = ""
        if instream is not None:
            if not sys._compiled:
                if not isinstance(instream, str):
                    instream = instream.read()
            text = instream
        self._text = text
        self._pos = 0
        self.infile = infile
        self.posix = posix
        self.eof: str | None = None if posix else ""
        self.commenters = "#"
        self.wordchars = "abcdfeghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
        if self.posix:
            self.wordchars += "ßàáâãäåæçèéêëìíîïðñòóôõöøùúûüýþÿÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖØÙÚÛÜÝÞ"
        self.whitespace = " \t\r\n"
        self.whitespace_split = False
        self.quotes = "'\""
        self.escape = "\\"
        self.escapedquotes = "\""
        self.state: str | None = " "
        self.pushback: list[str | None] = []
        self.lineno = 1
        self.debug = 0
        self.token = ""
        self.source: str | None = None
        pc = ""
        if isinstance(punctuation_chars, str):
            pc = punctuation_chars
        elif punctuation_chars:
            pc = "();<>|&"
        self._punctuation_chars = pc
        self._pushback_chars: list[str] = []
        if pc:
            self.wordchars += "~-./*?="
            self.wordchars = "".join([c for c in self.wordchars if c not in pc])

    @property
    def punctuation_chars(self) -> str:
        return self._punctuation_chars

    def _read1(self) -> str:
        if self._pos >= len(self._text):
            return ""
        c = self._text[self._pos]
        self._pos += 1
        return c

    def _readline(self) -> None:
        k = self._text.find("\n", self._pos)
        self._pos = len(self._text) if k < 0 else k + 1

    def push_token(self, tok: str | None) -> None:
        """tok will be get_token()'s next result."""
        self.pushback.insert(0, tok)

    def get_token(self) -> str | None:
        """The next token (from the pushed-back ones first); eof (None, or '' when not posix) at the end."""
        if self.pushback:
            return self.pushback.pop(0)
        return self.read_token()

    def read_token(self) -> str | None:
        quoted = False
        escapedstate = " "
        while True:
            nextchar = ""
            if self._punctuation_chars and self._pushback_chars:
                nextchar = self._pushback_chars.pop()
            else:
                nextchar = self._read1()
            if nextchar == "\n":
                self.lineno += 1
            st = self.state
            if st is None:
                self.token = ""
                break
            elif st == " ":
                if not nextchar:
                    self.state = None
                    break
                elif nextchar in self.whitespace:
                    if self.token or (self.posix and quoted):
                        break
                    else:
                        continue
                elif nextchar in self.commenters:
                    self._readline()
                    self.lineno += 1
                elif self.posix and nextchar in self.escape:
                    escapedstate = "a"
                    self.state = nextchar
                elif nextchar in self.wordchars:
                    self.token = nextchar
                    self.state = "a"
                elif nextchar in self._punctuation_chars:
                    self.token = nextchar
                    self.state = "c"
                elif nextchar in self.quotes:
                    if not self.posix:
                        self.token = nextchar
                    self.state = nextchar
                elif self.whitespace_split:
                    self.token = nextchar
                    self.state = "a"
                else:
                    self.token = nextchar
                    if self.token or (self.posix and quoted):
                        break
                    else:
                        continue
            elif st in self.quotes:
                quoted = True
                if not nextchar:
                    raise ValueError("No closing quotation")
                if nextchar == st:
                    if not self.posix:
                        self.token += nextchar
                        self.state = " "
                        break
                    else:
                        self.state = "a"
                elif self.posix and nextchar in self.escape and st in self.escapedquotes:
                    escapedstate = st
                    self.state = nextchar
                else:
                    self.token += nextchar
            elif st in self.escape:
                if not nextchar:
                    raise ValueError("No escaped character")
                if escapedstate in self.quotes and nextchar != st and nextchar != escapedstate:
                    self.token += st
                self.token += nextchar
                self.state = escapedstate
            elif st == "a" or st == "c":
                if not nextchar:
                    self.state = None
                    break
                elif nextchar in self.whitespace:
                    self.state = " "
                    if self.token or (self.posix and quoted):
                        break
                    else:
                        continue
                elif nextchar in self.commenters:
                    self._readline()
                    self.lineno += 1
                    if self.posix:
                        self.state = " "
                        if self.token or (self.posix and quoted):
                            break
                        else:
                            continue
                elif st == "c":
                    if nextchar in self._punctuation_chars:
                        self.token += nextchar
                    else:
                        if nextchar not in self.whitespace:
                            self._pushback_chars.append(nextchar)
                        self.state = " "
                        break
                elif self.posix and nextchar in self.quotes:
                    self.state = nextchar
                elif self.posix and nextchar in self.escape:
                    escapedstate = "a"
                    self.state = nextchar
                elif (nextchar in self.wordchars or nextchar in self.quotes or
                      (self.whitespace_split and nextchar not in self._punctuation_chars)):
                    self.token += nextchar
                else:
                    if self._punctuation_chars:
                        self._pushback_chars.append(nextchar)
                    else:
                        self.pushback.insert(0, nextchar)
                    self.state = " "
                    if self.token or (self.posix and quoted):
                        break
                    else:
                        continue
        result: str | None = self.token
        self.token = ""
        if self.posix and not quoted and result == "":
            result = None
        return result

    def error_leader(self, infile: str | None = None, lineno: int | None = None) -> str:
        """'"file", line N: ' for messages."""
        f = self.infile if infile is None else infile
        n = self.lineno if lineno is None else lineno
        return "\"" + str(f) + "\", line " + str(n) + ": "

    def __iter__(self):
        while True:
            token = self.get_token()
            if token == self.eof:
                return
            yield token


def split(s: str, comments: bool = False, posix: bool = True) -> list[str]:
    """s split as a POSIX shell splits words (quotes and backslashes; # comments if comments)."""
    if s is None:
        raise ValueError("s argument must not be None")
    lex = shlex(s, posix=posix)
    lex.whitespace_split = True
    if not comments:
        lex.commenters = ""
    out: list[str] = []
    while True:
        tok = lex.get_token()
        if tok == lex.eof or tok is None:
            break
        out.append(tok)
    return out


_SAFE = "%+,-./0123456789:=@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz"


def quote(s: str) -> str:
    """s quoted for a POSIX shell (as is when that is safe)."""
    if not s:
        return "''"
    safe = True
    for c in s:
        if c not in _SAFE:
            safe = False
            break
    if safe:
        return s
    return "'" + s.replace("'", "'\"'\"'") + "'"


def join(split_command: list[str]) -> str:
    """The words quoted and joined with spaces (split's inverse)."""
    return " ".join([quote(arg) for arg in split_command])
