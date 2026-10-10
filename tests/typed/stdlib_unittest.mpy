# unittest: test cases, fixtures, the assert methods' messages, skips, expected failures, subtests,
# suites, the loader, the text runner and main() (its report: the failing assert's line, as CPython's)
import io
import re
import sys
import unittest

log: list[str] = []


def setUpModule() -> None:
    log.append("setUpModule")


def tearDownModule() -> None:
    log.append("tearDownModule")


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        log.append("setUpClass")

    @classmethod
    def tearDownClass(cls) -> None:
        log.append("tearDownClass")

    def setUp(self) -> None:
        log.append("setUp " + self._testMethodName)
        self.addCleanup(log.append, "cleanup " + self._testMethodName)

    def tearDown(self) -> None:
        log.append("tearDown " + self._testMethodName)

    def test_equal(self) -> None:
        """Equality of numbers.

        More text."""
        self.assertEqual(1 + 1, 2)
        self.assertNotEqual(1, 2)
        self.assertTrue([1])
        self.assertFalse("")
        self.assertIs(None, None)
        self.assertIsNone(None)
        self.assertIsNotNone(0)
        self.assertIn(2, [1, 2])
        self.assertNotIn(3, [1, 2])
        self.assertIn("ell", "hello")
        self.assertIsInstance(1, int)
        self.assertNotIsInstance("x", int)
        self.assertAlmostEqual(0.1 + 0.2, 0.3)
        self.assertAlmostEqual(1.0, 1.05, delta=0.1)
        self.assertNotAlmostEqual(1.0, 1.1)
        self.assertGreater(2, 1)
        self.assertGreaterEqual(2, 2)
        self.assertLess(1, 2)
        self.assertLessEqual(1, 1)
        self.assertCountEqual([1, 2, 2], [2, 1, 2])
        self.assertRegex("hello", "l+")
        self.assertNotRegex("hello", "z")
        self.assertStartsWith("hello", "he")
        self.assertEndsWith("hello", "lo")
        self.assertListEqual([1, 2], [1, 2])
        self.assertTupleEqual((1, "a"), (1, "a"))
        self.assertSetEqual({1, 2}, {2, 1})
        self.assertDictEqual({"a": 1}, {"a": 1})
        self.assertMultiLineEqual("a\nb", "a\nb")
        self.assertEqual((1, "a"), (1, "a"))

    def test_raises(self) -> None:
        with self.assertRaises(ValueError) as cm:
            int("x")
        self.assertEqual(str(cm.exception), "invalid literal for int() with base 10: 'x'")
        self.assertRaises(ZeroDivisionError, lambda: 1 // 0)
        self.assertRaises(ValueError, int, "y")
        with self.assertRaisesRegex(KeyError, "miss"):
            {"a": 1}["missing"]

    def test_fail_dict(self) -> None:
        self.assertEqual({"a": 1, "b": 2}, {"a": 1, "b": 3})

    def test_fail_str(self) -> None:
        self.assertEqual("line one\nline two\n", "line one\nline 2\n")

    def test_fail_short_str(self) -> None:
        self.assertEqual("spam", "eggs")

    def test_fail_long_str(self) -> None:
        self.assertEqual("x" * 60 + "a" + "y" * 40, "x" * 60 + "b" + "y" * 40)

    def test_fail_msg(self) -> None:
        self.assertTrue(0, "zero is false")

    def test_fail_almost(self) -> None:
        self.assertAlmostEqual(1.0, 1.1, places=3)

    def test_fail_raises(self) -> None:
        with self.assertRaises(KeyError):
            pass

    def test_fail_raises_regex(self) -> None:
        with self.assertRaisesRegex(ValueError, "^nothing"):
            int("z")

    def test_fail_in(self) -> None:
        self.assertIn(5, [1, 2, 3])

    def test_fail_is(self) -> None:
        self.assertIsNone(7)

    def test_fail_greater(self) -> None:
        self.assertGreater(1, 2, "not bigger")

    def test_fail_isinstance(self) -> None:
        self.assertIsInstance("s", int)

    def test_subtests(self) -> None:
        for i in range(4):
            with self.subTest(i=i):
                self.assertEqual(i % 2, 0)
        with self.subTest("named"):
            self.assertTrue(False)

    @unittest.expectedFailure
    def test_xfail(self) -> None:
        self.assertEqual(1, 2)

    @unittest.expectedFailure
    def test_xpass(self) -> None:
        pass

    @unittest.skipIf(sys.platform != "nowhere", "not here")
    def test_skipif(self) -> None:
        pass

    @unittest.skipUnless(sys.platform == "nowhere", "only there")
    def test_skipunless(self) -> None:
        pass

    def test_skiptest(self) -> None:
        self.skipTest("later")

    def test_error(self) -> None:
        [][1]


class Derived(Base):
    def test_derived(self) -> None:
        self.assertEqual(len(self.id()), 35)

    def test_equal(self) -> None:
        self.assertEqual("over", "over")


@unittest.skip("whole class")
class Skipped(unittest.TestCase):
    def test_one(self) -> None:
        log.append("never")


class Other(unittest.TestCase):
    def test_list(self) -> None:
        self.assertListEqual([1, 2, 3], [1, 2, 3])

    def test_fail_list(self) -> None:
        self.assertEqual([1, 2, 3], [1, 2])

    def test_fail_list2(self) -> None:
        self.assertEqual([1, 2], [1, 2, 5])

    def test_fail_tuple(self) -> None:
        self.assertEqual((1, "a"), (1, "b"))

    def test_fail_set(self) -> None:
        self.assertEqual({1, 2}, {2, 3})

    def test_fail_count(self) -> None:
        self.assertCountEqual([1, 1, 2], [1, 2, 2])

    def test_fail_not_equal(self) -> None:
        self.assertNotEqual("a", "a")


def report(text: str) -> str:
    """(times, paths and line numbers left out; the traceback lines of errors too: compiled programs report
    where an assert failed, not where another exception was raised)"""
    text = re.sub(r"Ran (\d+) tests? in [0-9.]+s", r"Ran \1 tests", text)
    text = re.sub(r'File "[^"]*", line \d+', 'File "...", line N', text)
    text = re.sub(r'Traceback \(most recent call last\):\n(?:  .*\n)+(?!AssertionError)(?=\w)',
                  'Traceback (most recent call last):\n  ...\n', text)
    return "\n".join(["| " + line for line in text.split("\n")])     # (as data: not a traceback of the run)


stream = io.StringIO()
runner = unittest.TextTestRunner(stream=stream, verbosity=2)
loader = unittest.TestLoader()
suite = loader.loadTestsFromTestCase(Base)
suite.addTests(loader.loadTestsFromTestCase(Derived))
suite.addTests(loader.loadTestsFromTestCase(Other))
suite.addTest(Skipped("test_one"))
print(suite.countTestCases())
result = runner.run(suite)
print(report(stream.getvalue()))
print(result.testsRun, len(result.failures), len(result.errors), len(result.skipped),
      len(result.expectedFailures), len(result.unexpectedSuccesses), result.wasSuccessful())
print([str(t) for t, _ in result.errors])
print([(str(t), r) for t, r in result.skipped])
print("\n".join(log))
print(loader.getTestCaseNames(Other))
print(Base("test_equal").id(), Base("test_equal").shortDescription(), repr(Other("test_list")))

# dots, failfast
s1 = io.StringIO()
r1 = unittest.TextTestRunner(stream=s1, failfast=True).run(loader.loadTestsFromTestCase(Other))
print(report(s1.getvalue()))
print(r1.testsRun, r1.shouldStop)

# a test case and a suite run directly
res = unittest.TestResult()
Other("test_list").run(res)
unittest.TestSuite([Other("test_fail_set"), Other("test_list")]).run(res)
print(res.testsRun, len(res.failures), res.wasSuccessful(), res)
print(Other("test_list").run().wasSuccessful())


def check_answer() -> None:
    assert 6 * 7 == 42


ft = unittest.FunctionTestCase(check_answer, description="the answer")
print(ft, ft.shortDescription(), ft.run().wasSuccessful())

# main(): names and -k select tests
log.clear()
for argv in (["prog", "Other.test_list"], ["prog", "-k", "fail_set", "Other"], ["prog", "-q", "Derived.test_derived"]):
    s2 = io.StringIO()
    prog = unittest.main(argv=argv, exit=False, testRunner=unittest.TextTestRunner(stream=s2))
    r2 = prog.result
    print(argv, r2.testsRun, r2.wasSuccessful(), report(s2.getvalue()).split("\n")[0][2:])
print(log)
