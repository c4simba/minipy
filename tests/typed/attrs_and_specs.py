# getattr / hasattr / setattr by literal names; {width} / {precision} inside format specs
class Box:
    def __init__(self) -> None:
        self.size = 1

    def label(self) -> str:
        return "box"


b = Box()
print(getattr(b, "size"), hasattr(b, "size"), hasattr(b, "colour"), getattr(b, "colour", "none"), getattr(b, "label")())
setattr(b, "size", 9)
print(b.size, getattr(Box, "__name__"))
w = 7
p = 3
x = 2.718281828
print(f"[{x:{w}.{p}f}] [{x:<{w}.{p}}] [{'mid':^{w}}] [{'truncate':.{p}}] [{42:0{w}d}] [{x:{w + 3}.{p - 1}e}]")
for n in range(1, 4):
    print(f"{'#' * n:>{w}}|{n / 7:.{n}f}")
