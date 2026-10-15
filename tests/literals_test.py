# Group 10: number and string literals
# integer bases
print(0xff)
print(0xFF)
print(0o17)
print(0b1010)
print(0x10 + 0o10 + 0b10)
# underscores
print(1_000_000)
print(0xff_ff)
print(1_2_3)
# floats and exponents
print(3.14)
print(1e3)
print(2.5e-2)
print(1_000.5)
print(6.022e23)
# string escapes
print(len("\x41\x42\x43"))
print("\x41\x42\x43")
print("AB")
print(len("\t"))
print("a\tb")
print(len("\0"))
print("done" + "\r"[0:0])
# raw strings
print(r"a\nb")
print(len(r"\n"))
print(r"C:\Users\name")
# triple-quoted (single line)
print("""triple""")
print('''single triple''')
print("""with "quotes" inside""")
