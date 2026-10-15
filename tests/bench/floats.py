# Floating point: the Mandelbrot set on a W x H grid (complex arithmetic by
# hand) and a numeric integral of sin(x) * sqrt(x) - the math module's
# functions in a tight loop.
import math

W = 640
H = 480
MAXIT = 400
STEPS = 6_000_000


def mandelbrot() -> int:
    inside = 0
    for py in range(H):
        ci = -1.2 + 2.4 * py / H
        for px in range(W):
            cr = -2.1 + 3.0 * px / W
            zr = 0.0
            zi = 0.0
            it = 0
            while it < MAXIT and zr * zr + zi * zi <= 4.0:
                t = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = t
                it += 1
            if it == MAXIT:
                inside += 1
    return inside


def integral() -> float:
    a = 0.0
    b = math.pi
    h = (b - a) / STEPS
    s = 0.0
    for i in range(STEPS):
        x = a + (i + 0.5) * h
        s += math.sin(x) * math.sqrt(x)
    return s * h


print("floats", mandelbrot(), round(integral(), 6))
