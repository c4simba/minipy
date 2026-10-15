import math
import random
import time

print(math.sqrt(16), math.sqrt(2), math.pi, math.e, math.tau)
print(math.floor(2.7), math.ceil(2.1), math.floor(-2.5), math.ceil(-2.5), math.trunc(-2.7))
print(round(math.sin(math.pi / 2), 9), round(math.cos(0), 9), round(math.tan(math.pi / 4), 9))
print(round(math.atan(1) * 4, 9), round(math.atan2(1, 1), 9), round(math.atan2(-1, -1), 9))
print(round(math.asin(1), 9), round(math.acos(0), 9), round(math.acos(1), 9))
print(round(math.exp(1), 9), round(math.log(math.e), 9), math.log10(1000), math.log2(8), round(math.log(8, 2), 9))
print(math.pow(2, 10), math.hypot(3, 4), math.fabs(-3.5), math.fmod(7, 3), math.fmod(-7, 3))
print(math.degrees(math.pi), math.radians(180) == math.pi, math.gcd(48, 18), math.gcd(-12, 8), math.isqrt(17))
random.seed(42)
rolls = [random.randint(1, 6) for _ in range(200)]
print(min(rolls), max(rolls), len(rolls))
r = random.random()
print(0.0 <= r < 1.0)
u = random.uniform(5, 10)
print(5 <= u <= 10)
items = [1, 2, 3, 4, 5]
random.shuffle(items)
print(sorted(items))
print(random.choice(["only"]), random.randrange(1) == 0, random.choice("x"))
t0 = time.time()
time.sleep(0.02)
print(time.time() - t0 >= 0.015)
