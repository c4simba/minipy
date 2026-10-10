# heapq: the min-heap and max-heap functions, nlargest / nsmallest; bisect
import heapq
import bisect


def by_len(s: str) -> int:
    return len(s)


def main() -> None:
    h: list[int] = []
    for v in [5, 1, 8, 3, 9, 2, 7]:
        heapq.heappush(h, v)
    print(h[0], len(h), [heapq.heappop(h) for _ in range(3)])
    print(heapq.heappushpop(h, 0), heapq.heapreplace(h, 10), sorted(h))
    nums = [5, 1, 8, 3, 9, 2, 7]
    heapq.heapify(nums)
    print(nums[0], sorted(nums) == [1, 2, 3, 5, 7, 8, 9])
    print(heapq.nlargest(3, [5, 1, 8, 3, 9, 2]), heapq.nsmallest(2, [5, 1, 8, 3, 9, 2]))
    words = ["bb", "a", "ccc", "dd"]
    print(heapq.nlargest(2, words, key=by_len), heapq.nsmallest(2, words, key=by_len))
    print(heapq.nsmallest(5, [3, 1]), heapq.nlargest(0, [1]), heapq.nlargest(2, [1.5, -2.0, 3.25]))
    mh = [3, 1, 4, 1, 5, 9, 2, 6]
    heapq.heapify_max(mh)
    heapq.heappush_max(mh, 7)
    print(mh[0], [heapq.heappop_max(mh) for _ in range(4)])
    print(heapq.heappushpop_max(mh, 10), heapq.heapreplace_max(mh, 0), sorted(mh))
    tasks: list[tuple[int, str]] = []
    heapq.heappush(tasks, (2, "write"))
    heapq.heappush(tasks, (1, "plan"))
    heapq.heappush(tasks, (3, "test"))
    while tasks:
        pri, name = heapq.heappop(tasks)
        print(pri, name)
    try:
        heapq.heappop(tasks)
    except IndexError as e:
        print("IndexError", e)
    a = [1, 2, 4, 4, 8]
    print(bisect.bisect_left(a, 4), bisect.bisect_right(a, 4), bisect.bisect(a, 0))
    bisect.insort(a, 5)
    bisect.insort_left(a, 4)
    print(a)


main()
