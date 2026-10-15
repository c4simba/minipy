# Sierpinski triangle with brightness gradient
# Supported features: **, # comments, while, if, def, print

n = 6                     # size = 2^n
size = 2 ** n             # operator ** (exponentiation)

# Build the grid using nested loops (list comprehension is not supported)
grid = []
i = 0
while i < size:
    row = []
    j = 0
    while j < size:
        row.append(' ')
        j = j + 1
    grid.append(row)
    i = i + 1

gradient = ' .:-=+*#@'    # 9 brightness levels

def fill(x, y, s, depth):
    if s == 1:
        idx = depth
        if idx >= len(gradient):
            idx = len(gradient) - 1
        grid[y][x] = gradient[idx]
    else:
        half = s // 2
        fill(x, y, half, depth + 1)
        fill(x + half, y, half, depth + 1)
        fill(x + half, y + half, half, depth + 1)

fill(0, 0, size, 0)

# Print row by row
i = 0
while i < size:
    line = ''
    j = 0
    while j < size:
        line = line + grid[i][j]
        j = j + 1
    print(line)
    i = i + 1