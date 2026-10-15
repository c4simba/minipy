import difflib
s = difflib.SequenceMatcher(None, "abxcd", "abcd")
print(s.ratio(), s.quick_ratio(), s.real_quick_ratio(), s.get_matching_blocks())
print(s.get_opcodes())
print(s.find_longest_match(0, 5, 0, 4))
a = ["one\n", "two\n", "three\n", "four\n", "five\n"]
b = ["zero\n", "one\n", "tree\n", "four\n", "five\n", "six\n"]
print("".join(difflib.unified_diff(a, b, fromfile="a.txt", tofile="b.txt")))
print("".join(difflib.context_diff(a, b, fromfile="a.txt", tofile="b.txt")))
print("".join(difflib.ndiff(a, b)))
print(difflib.get_close_matches("appel", ["ape", "apple", "peach", "puppy"]))
print(difflib.get_close_matches("wheel", ["while", "wheat", "wheels", "whale"], n=2, cutoff=0.5))
d = difflib.Differ()
print("".join(d.compare(["abcDefghiJkl\n"], ["abcdefGhijkl\n"])))
print("".join(difflib.restore(difflib.ndiff(a, b), 2)))
sm = difflib.SequenceMatcher(lambda x: x == " ", "private Thread currentThread;", "private volatile Thread currentThread;")
print(round(sm.ratio(), 3), list(sm.get_grouped_opcodes(1)))
print(difflib.IS_CHARACTER_JUNK(" "), difflib.IS_LINE_JUNK("  #  \n"), difflib.IS_LINE_JUNK("x\n"))
