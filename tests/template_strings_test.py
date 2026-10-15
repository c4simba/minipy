from string.templatelib import Template, Interpolation
import string
name = "World"; n = 3.14159
t = t"Hello {name}! pi={n:.2f} r={name!r}"
print(repr(t))
print(t.strings, t.values)
print(list(t))
print(repr(t"" ), repr(t"{name}"), repr(t"a" + t"{n}"))
print(type(t).__name__, type(t.interpolations[0]).__name__, t.interpolations[0].expression, t.interpolations[1].format_spec, t.interpolations[2].conversion)
i = Interpolation(5, "x")
print(repr(i), repr(Template("a", i, "b")))
try:
    print("x" + t)
except TypeError as e: print(e)
match t.interpolations[0]:
    case Interpolation(value, expr):
        print(value, expr)
def render(tpl: Template) -> str:
    out = []
    for part in tpl:
        if isinstance(part, str):
            out.append(part)
        else:
            v = part.value
            if part.conversion == "r":
                v = repr(v)
            out.append(format(v, part.format_spec))
    return "".join(out)
print(render(t))
w = 8
print(repr(t"{name:>{w}}"), string.ascii_lowercase[:5], string.capwords("hello big world"), len(string.punctuation))
