# shlex: split (posix, comments), quote, join, the lexer
import shlex

for s in ['ls -l "my file.txt" \'single q\' a\\ b', 'echo "a \\"quoted\\" word" x#y # comment', "a'b'c \"d\"e", 'one  two\tthree\nfour',
          'x="a b" y=\'c\'', 'esc "\\$HOME \\x" \'\\n\'', '', '   ', 'a"b"c']:
    print(repr(s), shlex.split(s), shlex.split(s, comments=True))
print(shlex.split("a 'b c' d", posix=False))
for w in ["simple", "with space", "it's", "", "$var", "a\"b", "safe-chars_./=@%+,:"]:
    print(repr(w), shlex.quote(w))
print(shlex.join(["echo", "hello world", "it's"]))
lex = shlex.shlex("a = b; c(d, 'e f')")
toks = []
while True:
    t = lex.get_token()
    if t == lex.eof:
        break
    toks.append(t)
print(toks)
lex2 = shlex.shlex("cmd && other || x; y", punctuation_chars=True)
print(list(lex2))
lex3 = shlex.shlex("a b", posix=True)
print(lex3.get_token(), lex3.get_token(), lex3.get_token())
try:
    shlex.split('unclosed "quote')
except ValueError as e:
    print("ValueError", e)
try:
    shlex.split('trailing \\')
except ValueError as e:
    print("ValueError", e)
