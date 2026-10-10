from html.parser import HTMLParser


class Show(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.events.append("start " + tag + " " + repr(attrs))

    def handle_endtag(self, tag: str) -> None:
        self.events.append("end " + tag)

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.events.append("startend " + tag + " " + repr(attrs))

    def handle_data(self, data: str) -> None:
        self.events.append("data " + repr(data))

    def handle_comment(self, data: str) -> None:
        self.events.append("comment " + repr(data))

    def handle_decl(self, decl: str) -> None:
        self.events.append("decl " + repr(decl))

    def handle_charref(self, name: str) -> None:
        self.events.append("charref " + name)

    def handle_entityref(self, name: str) -> None:
        self.events.append("entityref " + name)

    def handle_pi(self, data: str) -> None:
        self.events.append("pi " + repr(data))


p = Show()
p.feed('<!DOCTYPE html><html><head><title>T &amp; x</title></head>')
p.feed('<body class="main" id=b1 hidden><p>Hi&nbsp;<b>there</b><br/><img src="a.png" alt=\'A\'>')
p.feed('<!-- note --><?php x ?><script>if (a < b) { }</script></body></html>')
p.close()
for e in p.events:
    print(e)
print(p.getpos())
q = Show()
q.convert_charrefs = False
q.feed("a &lt; b &#62; c")
q.close()
print(q.events)
