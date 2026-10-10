import html
from html.entities import name2codepoint, codepoint2name, html5
print(html.escape('<a href="x">Tom & \'Jerry\'</a>'), html.escape("<b>", quote=False))
print(html.unescape("&lt;p&gt; &amp;amp; &#62;&#x3e;&#X3E; &copy;2024 &notit; &nosuchthing; &#0; &#xD800; &#128; &eacute"))
print(name2codepoint["amp"], codepoint2name[60], html5["gt;"], len(html5) > 2000)
