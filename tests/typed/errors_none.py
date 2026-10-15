class Node:
    def __init__(self, v: int):
        self.v = v
        self.next: Node = None

n = Node(1)
print(n.v)
print(n.next.v)
