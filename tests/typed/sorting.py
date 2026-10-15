# key= and reverse= for sorted / list.sort, key= for min / max
words = ["banana", "Apple", "cherry", "fig", "date", "Elder"]
print(sorted(words), sorted(words, reverse=True))
print(sorted(words, key=len), sorted(words, key=lambda w: w.lower()))
print(sorted(words, key=len, reverse=True))
print(sorted(words, key=lambda w: (len(w), w)))

class Person:
    def __init__(self, name: str, age: int):
        self.name = name
        self.age = age

    def __repr__(self) -> str:
        return self.name + "(" + str(self.age) + ")"

def by_age(p: Person) -> int:
    return p.age

people = [Person("ann", 31), Person("bob", 25), Person("cy", 31), Person("dee", 19)]
print(sorted(people, key=by_age), sorted(people, key=by_age, reverse=True))
people.sort(key=lambda p: p.name, reverse=True)
print(people)
print(min(people, key=by_age), max(people, key=lambda p: p.age), max(words, key=len), min(words, key=len))
nums = [3, -7, 2, -1, 5]
nums.sort(key=abs)
print(nums)
nums.sort(reverse=True)
print(nums, sorted([2.5, -1.5, 0.5], key=lambda x: -x))
pairs = [("b", 2), ("a", 2), ("c", 1)]
print(sorted(pairs, key=lambda kv: kv[1]), max(pairs, key=lambda kv: kv[1]))
scores = {"x": 3, "y": 9, "z": 1}
print(sorted(scores, key=lambda k: scores[k]), max(scores.items(), key=lambda kv: kv[1]))
