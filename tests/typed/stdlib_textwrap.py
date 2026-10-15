# textwrap (CPython's word splitting)
import textwrap
from textwrap import TextWrapper

text = "Look, goof-ball -- use the -b option! The quick brown fox jumps over the lazy dog. Self-explanatory well-known x-ray e-mail foo--bar a-b-c-d co-op 123-456 re-un-done."
for w in [10, 20, 33, 70]:
    print(w, textwrap.wrap(text, w))
print(TextWrapper()._split("Look, goof-ball -- use the -b option!"))
print(TextWrapper(break_on_hyphens=False)._split("Look, goof-ball -- use the -b option!"))
print(textwrap.fill(text, 25, initial_indent="* ", subsequent_indent="  "))
print(textwrap.wrap("supercalifragilisticexpialidocious and-more-hyphenated-words", 12))
print(textwrap.wrap("supercalifragilisticexpialidocious", 12, break_long_words=False))
print(textwrap.wrap("Hello.  World. Ok. End?", 40, fix_sentence_endings=True), textwrap.wrap("Hello. World.\nNext", 40, fix_sentence_endings=True))
print(textwrap.shorten("Hello  world!", width=12), textwrap.shorten("Hello  world!", width=11), textwrap.shorten(text, 40), textwrap.shorten(text, 40, placeholder="..."))
print(textwrap.wrap(text, 30, max_lines=3), textwrap.wrap("a\tb\tc", 20), textwrap.wrap("a\tb\tc", 20, expand_tabs=False), textwrap.wrap("  lead  and trail  ", 10, drop_whitespace=False))
print(repr(textwrap.dedent("    hello\n      world\n    \n    end")), repr(textwrap.dedent("\tx\n  y")), repr(textwrap.dedent("  a\n  b\n")))
print(repr(textwrap.indent("a\n\nb\n  \nc", "> ")), repr(textwrap.indent("a\nb", "+ ", lambda line: line.startswith("b"))))
try:
    textwrap.wrap("x", 0)
except ValueError as e:
    print("ValueError", e)
print(textwrap.wrap("", 10), textwrap.fill("one two three four five six seven", 9))
