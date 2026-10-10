"""Text wrapping and filling (CPython's textwrap): wrap, fill, shorten, dedent, indent
and TextWrapper. Words split as CPython's regular expressions split them."""

__all__ = ["TextWrapper", "wrap", "fill", "dedent", "indent", "shorten"]

_whitespace = "\t\n\x0b\x0c\r "


def _is_w(c: str) -> bool:
    return c.isalnum() or c == "_"


def _is_letter(c: str) -> bool:
    return (c.isalnum() or c == "_") and not c.isdigit()


def _is_wp(c: str) -> bool:
    return _is_w(c) or c in "!\"'&.,?"


def _dashes_then_word(text: str, j: int) -> bool:
    """text[j:] is -{2,} and a word character (as the regex tries it)."""
    n = len(text)
    k = j
    while k < n and text[k] == "-":
        k += 1
    return k - j >= 2 and k < n and _is_w(text[k])


def _split_hyphens(text: str) -> list[str]:
    """The chunks of CPython's wordsep_re: whitespace runs, em-dashes, words (split after inner hyphens)."""
    chunks: list[str] = []
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c in _whitespace:
            j = i
            while j < n and text[j] in _whitespace:
                j += 1
            chunks.append(text[i:j])
            i = j
            continue
        if c == "-" and i > 0 and _is_wp(text[i - 1]) and _dashes_then_word(text, i):
            j = i
            while j < n and text[j] == "-":
                j += 1
            chunks.append(text[i:j])
            i = j
            continue
        j = i + 1                                   # a word: the shortest run of non-whitespace that ends right
        end = -1
        while True:
            if j < n and text[j] == "-":
                before = (j >= 2 and _is_letter(text[j - 2]) and _is_letter(text[j - 1])) or \
                         (j >= 3 and _is_letter(text[j - 3]) and text[j - 2] == "-" and _is_letter(text[j - 1]))
                after = j + 1 < n and _is_letter(text[j + 1]) and \
                    ((j + 2 < n and _is_letter(text[j + 2])) or (j + 3 < n and text[j + 2] == "-" and _is_letter(text[j + 3])))
                if before and after:
                    end = j + 1
                    break
            if j >= n or text[j] in _whitespace:
                end = j
                break
            if _is_wp(text[j - 1]) and _dashes_then_word(text, j):
                end = j
                break
            j += 1
        chunks.append(text[i:end])
        i = end
    return chunks


def _split_simple(text: str) -> list[str]:
    chunks: list[str] = []
    n = len(text)
    i = 0
    while i < n:
        j = i
        ws = text[i] in _whitespace
        while j < n and (text[j] in _whitespace) == ws:
            j += 1
        chunks.append(text[i:j])
        i = j
    return chunks


def _sentence_end(chunk: str) -> bool:
    """[a-z][.!?]["']? at the end."""
    k = len(chunk)
    if k and chunk[k - 1] in "\"'":
        k -= 1
    return k >= 2 and chunk[k - 1] in ".!?" and "a" <= chunk[k - 2] <= "z"


class TextWrapper:
    """Wraps text: wrap() its lines, fill() them joined by newlines."""

    def __init__(self, width: int = 70, initial_indent: str = "", subsequent_indent: str = "", expand_tabs: bool = True,
                 replace_whitespace: bool = True, fix_sentence_endings: bool = False, break_long_words: bool = True,
                 drop_whitespace: bool = True, break_on_hyphens: bool = True, tabsize: int = 8, *,
                 max_lines: int | None = None, placeholder: str = " [...]"):
        self.width = width
        self.initial_indent = initial_indent
        self.subsequent_indent = subsequent_indent
        self.expand_tabs = expand_tabs
        self.replace_whitespace = replace_whitespace
        self.fix_sentence_endings = fix_sentence_endings
        self.break_long_words = break_long_words
        self.drop_whitespace = drop_whitespace
        self.break_on_hyphens = break_on_hyphens
        self.tabsize = tabsize
        self.max_lines = max_lines
        self.placeholder = placeholder

    def _munge_whitespace(self, text: str) -> str:
        if self.expand_tabs:
            text = text.expandtabs(self.tabsize)
        if self.replace_whitespace:
            for c in "\t\n\x0b\x0c\r":
                text = text.replace(c, " ")
        return text

    def _split(self, text: str) -> list[str]:
        if self.break_on_hyphens:
            return _split_hyphens(text)
        return _split_simple(text)

    def _fix_sentence_endings(self, chunks: list[str]) -> None:
        i = 0
        while i < len(chunks) - 1:
            if chunks[i + 1] == " " and _sentence_end(chunks[i]):
                chunks[i + 1] = "  "
                i += 2
            else:
                i += 1

    def _handle_long_word(self, reversed_chunks: list[str], cur_line: list[str], cur_len: int, width: int) -> None:
        space_left = 1 if width < 1 else width - cur_len
        if self.break_long_words and space_left > 0:
            end = space_left
            chunk = reversed_chunks[-1]
            if self.break_on_hyphens and len(chunk) > space_left:
                hyphen = chunk.rfind("-", 0, space_left)
                if hyphen > 0 and any(c != "-" for c in chunk[:hyphen]):
                    end = hyphen + 1
            cur_line.append(chunk[:end])
            reversed_chunks[-1] = chunk[end:]
        elif not cur_line:
            cur_line.append(reversed_chunks.pop())

    def _wrap_chunks(self, chunks: list[str]) -> list[str]:
        lines: list[str] = []
        if self.width <= 0:
            raise ValueError("invalid width " + repr(self.width) + " (must be > 0)")
        max_lines = self.max_lines
        if max_lines is not None:
            indent = self.subsequent_indent if max_lines > 1 else self.initial_indent
            if len(indent) + len(self.placeholder.lstrip()) > self.width:
                raise ValueError("placeholder too large for max width")
        chunks.reverse()
        while chunks:
            cur_line: list[str] = []
            cur_len = 0
            indent = self.subsequent_indent if lines else self.initial_indent
            width = self.width - len(indent)
            if self.drop_whitespace and chunks[-1].strip() == "" and lines:
                del chunks[-1]
            while chunks:
                l = len(chunks[-1])
                if cur_len + l <= width:
                    cur_line.append(chunks.pop())
                    cur_len += l
                else:
                    break
            if chunks and len(chunks[-1]) > width:
                self._handle_long_word(chunks, cur_line, cur_len, width)
                cur_len = sum(len(x) for x in cur_line)
            if self.drop_whitespace and cur_line and cur_line[-1].strip() == "":
                cur_len -= len(cur_line[-1])
                del cur_line[-1]
            if cur_line:
                if (max_lines is None or len(lines) + 1 < max_lines
                        or (not chunks or self.drop_whitespace and len(chunks) == 1 and not chunks[0].strip())
                        and cur_len <= width):
                    lines.append(indent + "".join(cur_line))
                else:
                    done = False
                    while cur_line:
                        if cur_line[-1].strip() and cur_len + len(self.placeholder) <= width:
                            cur_line.append(self.placeholder)
                            lines.append(indent + "".join(cur_line))
                            done = True
                            break
                        cur_len -= len(cur_line[-1])
                        del cur_line[-1]
                    if not done:
                        if lines:
                            prev_line = lines[-1].rstrip()
                            if len(prev_line) + len(self.placeholder) <= self.width:
                                lines[-1] = prev_line + self.placeholder
                                break
                        lines.append(indent + self.placeholder.lstrip())
                    break
        return lines

    def _split_chunks(self, text: str) -> list[str]:
        return self._split(self._munge_whitespace(text))

    def wrap(self, text: str) -> list[str]:
        """text's lines of at most width characters (no newlines in them)."""
        chunks = self._split_chunks(text)
        if self.fix_sentence_endings:
            self._fix_sentence_endings(chunks)
        return self._wrap_chunks(chunks)

    def fill(self, text: str) -> str:
        """wrap() joined with newlines."""
        return "\n".join(self.wrap(text))


def wrap(text: str, width: int = 70, *, initial_indent: str = "", subsequent_indent: str = "", expand_tabs: bool = True,
         replace_whitespace: bool = True, fix_sentence_endings: bool = False, break_long_words: bool = True,
         drop_whitespace: bool = True, break_on_hyphens: bool = True, tabsize: int = 8, max_lines: int | None = None,
         placeholder: str = " [...]") -> list[str]:
    """The lines of a paragraph wrapped to width columns."""
    return TextWrapper(width, initial_indent, subsequent_indent, expand_tabs, replace_whitespace, fix_sentence_endings,
                       break_long_words, drop_whitespace, break_on_hyphens, tabsize, max_lines=max_lines,
                       placeholder=placeholder).wrap(text)


def fill(text: str, width: int = 70, *, initial_indent: str = "", subsequent_indent: str = "", expand_tabs: bool = True,
         replace_whitespace: bool = True, fix_sentence_endings: bool = False, break_long_words: bool = True,
         drop_whitespace: bool = True, break_on_hyphens: bool = True, tabsize: int = 8, max_lines: int | None = None,
         placeholder: str = " [...]") -> str:
    """A paragraph wrapped to width columns, its lines joined with newlines."""
    return TextWrapper(width, initial_indent, subsequent_indent, expand_tabs, replace_whitespace, fix_sentence_endings,
                       break_long_words, drop_whitespace, break_on_hyphens, tabsize, max_lines=max_lines,
                       placeholder=placeholder).fill(text)


def shorten(text: str, width: int, *, initial_indent: str = "", subsequent_indent: str = "", expand_tabs: bool = True,
            replace_whitespace: bool = True, fix_sentence_endings: bool = False, break_long_words: bool = True,
            drop_whitespace: bool = True, break_on_hyphens: bool = True, tabsize: int = 8,
            placeholder: str = " [...]") -> str:
    """text with its whitespace collapsed, cut to width with placeholder at the end."""
    return TextWrapper(width, initial_indent, subsequent_indent, expand_tabs, replace_whitespace, fix_sentence_endings,
                       break_long_words, drop_whitespace, break_on_hyphens, tabsize, max_lines=1,
                       placeholder=placeholder).fill(" ".join(text.strip().split()))


def dedent(text: str) -> str:
    """text without the leading whitespace all its lines have; whitespace-only lines emptied."""
    lines = text.split("\n")
    non_blank = [l for l in lines if l and not l.isspace()]
    l1 = min(non_blank) if non_blank else ""
    l2 = max(non_blank) if non_blank else ""
    margin = 0
    for k in range(len(l1)):
        margin = k
        if l1[k] != l2[k] or l1[k] not in " \t":
            break
    return "\n".join([l[margin:] if not l.isspace() else "" for l in lines])


def indent(text: str, prefix: str, predicate=None) -> str:
    """text with prefix before each line (with predicate: each line it is true for; else the non-blank ones)."""
    out: list[str] = []
    for line in text.splitlines(True):
        if predicate is None:
            if not line.isspace():
                out.append(prefix)
        else:
            if predicate(line):
                out.append(prefix)
        out.append(line)
    return "".join(out)
