"""
Python unit testing framework, based on Erich Gamma's JUnit and Kent Beck's
Smalltalk testing framework (used with permission).

This module contains the core framework classes that form the basis of
specific test cases and suites (TestCase, TestSuite etc.), and also a
text-based utility class for running the tests and reporting the results
 (TextTestRunner).

Simple usage:

    import unittest

    class IntegerArithmeticTestCase(unittest.TestCase):
        def testAdd(self):  # test method names begin with 'test'
            self.assertEqual((1 + 2), 3)
            self.assertEqual(0 + 1, 1)
        def testMultiply(self):
            self.assertEqual((0 * 10), 0)
            self.assertEqual((5 * 8), 40)

    if __name__ == '__main__':
        unittest.main()

Further information is available in the bundled documentation, and from

  http://docs.python.org/library/unittest.html

Compiled programs (minipy): the tests (the TestCase subclasses of the program and their test*
methods, the skip / expectedFailure decorators) are found when the program is compiled; a
failure's report gives the line of the failing assert*() call (an error's: the test method),
not a whole traceback. assertRaises(exc, f, *args) takes arguments of one type; subTest() keyword
values of one type; no test discovery, assertWarns, assertLogs or unittest.mock.

Copyright (c) 1999-2003 Steve Purcell
Copyright (c) 2003 Python Software Foundation
This module is free software, and you may redistribute it and/or modify
it under the same terms as Python itself, so long as this copyright message
and disclaimer are retained in their original form.

IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF
THIS CODE, EVEN IF THE AUTHOR HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE.  THE CODE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS,
AND THERE IS NO OBLIGATION WHATSOEVER TO PROVIDE MAINTENANCE,
SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
"""

import sys

if not sys._compiled:
    __all__ = ['TestResult', 'TestCase', 'IsolatedAsyncioTestCase', 'TestSuite',
               'TextTestRunner', 'TestLoader', 'FunctionTestCase', 'main',
               'defaultTestLoader', 'SkipTest', 'skip', 'skipIf', 'skipUnless',
               'expectedFailure', 'TextTestResult', 'installHandler',
               'registerResult', 'removeResult', 'removeHandler',
               'addModuleCleanup', 'doModuleCleanups', 'enterModuleContext']

    __unittest = True

    from .result import TestResult
    from .case import (addModuleCleanup, TestCase, FunctionTestCase, SkipTest, skip,
                       skipIf, skipUnless, expectedFailure, doModuleCleanups,
                       enterModuleContext)
    from .suite import BaseTestSuite, TestSuite  # noqa: F401
    from .loader import TestLoader, defaultTestLoader
    from .main import TestProgram, main  # noqa: F401
    from .runner import TextTestRunner, TextTestResult
    from .signals import installHandler, registerResult, removeResult, removeHandler
    # IsolatedAsyncioTestCase will be imported lazily.


    # Lazy import of IsolatedAsyncioTestCase from .async_case
    # It imports asyncio, which is relatively heavy, but most tests
    # do not need it.

    def __dir__():
        return globals().keys() | {'IsolatedAsyncioTestCase'}

    def __getattr__(name):
        if name == 'IsolatedAsyncioTestCase':
            global IsolatedAsyncioTestCase
            from .async_case import IsolatedAsyncioTestCase
            return IsolatedAsyncioTestCase
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

if sys._compiled:
    import difflib as _difflib
    import pprint as _pprint
    import re as _re
    import time as _time
    from typing import Callable, TypeVar

    _T = TypeVar("_T")
    _S = TypeVar("_S")
    _K = TypeVar("_K")
    _V = TypeVar("_V")
    _W = TypeVar("_W")
    _R = TypeVar("_R")

    __all__ = ['TestResult', 'TestCase', 'TestSuite', 'TextTestRunner', 'TestLoader', 'FunctionTestCase', 'main',
               'defaultTestLoader', 'SkipTest', 'skip', 'skipIf', 'skipUnless', 'expectedFailure',
               'TextTestResult', 'installHandler', 'registerResult', 'removeResult', 'removeHandler']

    class SkipTest(Exception):
        """Raise this exception in a test to skip it."""

    class _ShouldStop(Exception):
        """The test should stop."""

    DIFF_OMITTED = '\nDiff is %s characters long. Set self.maxDiff to None to see it.'
    _MAX_LENGTH = 80
    _PLACEHOLDER_LEN = 12
    _MIN_BEGIN_LEN = 5
    _MIN_END_LEN = 5
    _MIN_COMMON_LEN = 5
    _MIN_DIFF_LEN = _MAX_LENGTH - (_MIN_BEGIN_LEN + _PLACEHOLDER_LEN + _MIN_COMMON_LEN + _PLACEHOLDER_LEN + _MIN_END_LEN)
    _NO_TESTS_EXITCODE = 5

    def safe_repr(obj: _T, short: bool = False) -> str:
        result = repr(obj)
        if not short or len(result) < _MAX_LENGTH:
            return result
        return result[:_MAX_LENGTH] + ' [truncated]...'

    def _shorten(s: str, prefixlen: int, suffixlen: int) -> str:
        skip = len(s) - prefixlen - suffixlen
        if skip > _PLACEHOLDER_LEN:
            s = '%s[%d chars]%s' % (s[:prefixlen], skip, s[len(s) - suffixlen:])
        return s

    def _shorten_reprs(a: str, b: str) -> tuple[str, str]:
        """(_common_shorten_repr of two reprs)"""
        maxlen = max(len(a), len(b))
        if maxlen <= _MAX_LENGTH:
            return a, b
        n = 0
        while n < len(a) and n < len(b) and a[n] == b[n]:
            n += 1
        prefix = a[:n]
        prefixlen = n
        common_len = _MAX_LENGTH - (maxlen - prefixlen + _MIN_BEGIN_LEN + _PLACEHOLDER_LEN)
        if common_len > _MIN_COMMON_LEN:
            prefix = _shorten(prefix, _MIN_BEGIN_LEN, common_len)
            return prefix + a[prefixlen:], prefix + b[prefixlen:]
        prefix = _shorten(prefix, _MIN_BEGIN_LEN, _MIN_COMMON_LEN)
        return (prefix + _shorten(a[prefixlen:], _MIN_DIFF_LEN, _MIN_END_LEN),
                prefix + _shorten(b[prefixlen:], _MIN_DIFF_LEN, _MIN_END_LEN))

    def _common_shorten_repr(a: _T, b: _S) -> tuple[str, str]:
        return _shorten_reprs(safe_repr(a), safe_repr(b))

    def _exc_line(e: BaseException) -> str:
        """The last line of a traceback: Name: message."""
        name = type(e).__name__
        text = str(e)
        return name + ': ' + text if text else name

    # ---- decorators (the compiler reads them: see ut_generate)

    class _Skip:
        def __init__(self, reason: str | None) -> None:
            self.reason = reason

    def skip(reason: str) -> _Skip:
        """Unconditionally skip a test."""
        return _Skip(reason)

    def skipIf(condition: _T, reason: str) -> _Skip:
        """Skip a test if the condition is true."""
        return _Skip(reason if condition else None)

    def skipUnless(condition: _T, reason: str) -> _Skip:
        """Skip a test unless the condition is true."""
        return _Skip(None if condition else reason)

    def expectedFailure(test_item: _T) -> _T:
        return test_item

    # ---- results

    class _Outcome:
        def __init__(self, result: "TestResult") -> None:
            self.expecting_failure = False
            self.result = result
            self.success = True
            self.expectedFailure: BaseException | None = None

    class _TestItem:
        """(what a suite holds: a test case or a suite)"""

        def run(self, result: "TestResult") -> "TestResult":
            return result

        def countTestCases(self) -> int:
            return 0

        def __call__(self, result: "TestResult") -> "TestResult":
            return self.run(result)

    class TestResult:
        """Holder for test result information."""

        def __init__(self, stream: _W = None, descriptions: bool | None = None, verbosity: int | None = None) -> None:
            self.failfast = False
            self.failures: list[tuple[TestCase, str]] = []
            self.errors: list[tuple[TestCase, str]] = []
            self.testsRun = 0
            self.skipped: list[tuple[TestCase, str]] = []
            self.expectedFailures: list[tuple[TestCase, str]] = []
            self.unexpectedSuccesses: list[TestCase] = []
            self.collectedDurations: list[tuple[str, float]] = []
            self.shouldStop = False
            self.buffer = False
            self.tb_locals = False
            self._previousTest: TestCase | None = None
            self._testRunEntered = False
            self._moduleSetUpFailed = False
            self._classSetupFailed: dict[str, bool] = {}

        def printErrors(self) -> None:
            pass

        def startTest(self, test: "TestCase") -> None:
            self.testsRun += 1

        def startTestRun(self) -> None:
            pass

        def stopTest(self, test: "TestCase") -> None:
            pass

        def stopTestRun(self) -> None:
            pass

        def addError(self, test: "TestCase", err: BaseException) -> None:
            if self.failfast:
                self.stop()
            self.errors.append((test, self._exc_info_to_string(err, test)))

        def addFailure(self, test: "TestCase", err: BaseException) -> None:
            if self.failfast:
                self.stop()
            self.failures.append((test, self._exc_info_to_string(err, test)))

        def addSubTest(self, test: "TestCase", subtest: "TestCase", err: BaseException | None) -> None:
            if err is not None:
                if self.failfast:
                    self.stop()
                if isinstance(err, AssertionError):
                    self.failures.append((subtest, self._exc_info_to_string(err, test)))
                else:
                    self.errors.append((subtest, self._exc_info_to_string(err, test)))

        def addSuccess(self, test: "TestCase") -> None:
            pass

        def addSkip(self, test: "TestCase", reason: str) -> None:
            self.skipped.append((test, reason))

        def addExpectedFailure(self, test: "TestCase", err: BaseException) -> None:
            self.expectedFailures.append((test, self._exc_info_to_string(err, test)))

        def addUnexpectedSuccess(self, test: "TestCase") -> None:
            if self.failfast:
                self.stop()
            self.unexpectedSuccesses.append(test)

        def addDuration(self, test: "TestCase", elapsed: float) -> None:
            self.collectedDurations.append((str(test), elapsed))

        def wasSuccessful(self) -> bool:
            """Tells whether or not this result was a success."""
            return (len(self.failures) == len(self.errors) == 0 and
                    len(self.unexpectedSuccesses) == 0)

        def stop(self) -> None:
            """Indicates that the tests should be aborted."""
            self.shouldStop = True

        def _exc_info_to_string(self, err: BaseException, test: "TestCase") -> str:
            """The report of an exception: where (the failing assert's line, or the test method) and what."""
            where = '  File "' + __mpy_ut_file(test) + '", in ' + test._testMethodName + '\n'
            frame = test._failframe
            if not frame or not isinstance(err, AssertionError):
                frame = where
            text = 'Traceback (most recent call last):\n' + frame + _exc_line(err) + '\n'
            ctx = test._failcontext
            if ctx is not None and isinstance(err, AssertionError):
                text = (_exc_line(ctx) + '\n\nDuring handling of the above exception, another exception occurred:\n\n' +
                        text)                               # (assertRaises cleared its traceback)
            return text

        def __repr__(self) -> str:
            m = type(self).__module__
            if m == "unittest":
                m = "unittest.runner" if isinstance(self, TextTestResult) else "unittest.result"
            return ("<%s.%s run=%i errors=%i failures=%i>" %
                    (m, type(self).__qualname__, self.testsRun, len(self.errors), len(self.failures)))

    # ---- test cases

    class _SubTestContext:
        """with self.subTest(...): a failure inside is the subtest's; the test goes on after the block."""

        def __init__(self, case: "TestCase", sub: "_SubTest", parent: "_SubTest | None") -> None:
            self._case = case
            self._sub = sub
            self._parent = parent

        def __enter__(self) -> None:
            pass

        def __exit__(self, exc_type, exc_value, tb) -> bool:
            case = self._case
            case._subtest = self._parent
            outcome = case._outcome
            if outcome is None:
                return False
            if exc_value is None:
                if outcome.success:
                    outcome.result.addSubTest(case, self._sub, None)
                if outcome.expectedFailure is not None:
                    raise _ShouldStop()
                return False
            if isinstance(exc_value, KeyboardInterrupt) or isinstance(exc_value, _ShouldStop):
                return False
            if isinstance(exc_value, SkipTest):
                outcome.success = False
                outcome.result.addSkip(self._sub, str(exc_value))
                return True
            if outcome.expecting_failure:
                outcome.expectedFailure = exc_value
                raise _ShouldStop()
            outcome.success = False
            outcome.result.addSubTest(case, self._sub, exc_value)
            if outcome.result.failfast:
                raise _ShouldStop()
            return True

    class _AssertRaisesContext:
        """A context manager used to implement TestCase.assertRaises* methods."""

        def __init__(self, expected: type[BaseException], test_case: "TestCase", expected_regex: str | None,
                     at: str) -> None:
            self.expected = expected
            self.test_case = test_case
            self.expected_regex = expected_regex
            self.obj_name: str | None = None
            self.msg: str | None = None
            self.exception: BaseException | None = None
            self._at = at

        def __enter__(self) -> "_AssertRaisesContext":
            return self

        def __exit__(self, exc_type, exc_value, tb) -> bool:
            if exc_type is None:
                exc_name = self.expected.__name__
                if self.obj_name:
                    self.test_case._fail(self.test_case._formatMessage(
                        self.msg, "{} not raised by {}".format(exc_name, self.obj_name)), self._at)
                else:
                    self.test_case._fail(self.test_case._formatMessage(self.msg, "{} not raised".format(exc_name)),
                                         self._at)
            if not issubclass(exc_type, self.expected):
                return False
            self.exception = exc_value
            if self.expected_regex is None:
                return True
            if not _re.search(self.expected_regex, str(exc_value)):
                self.test_case._failcontext = exc_value
                self.test_case._fail(self.test_case._formatMessage(
                    self.msg, '"{}" does not match "{}"'.format(self.expected_regex, str(exc_value))), self._at)
            return True

    class TestCase(_TestItem):
        """A class whose instances are single test cases (one test method each)."""
        longMessage = True
        maxDiff: int | None = 80 * 8
        _diffThreshold = 2 ** 16

        def __init__(self, methodName: str = 'runTest') -> None:
            self._testMethodName = methodName
            self._outcome: _Outcome | None = None
            self._cleanups: list[Callable[[], None]] = []
            self._subtest: _SubTest | None = None
            self._failframe = ""
            self._failcontext: BaseException | None = None

        def addCleanup(self, function, *args) -> None:
            """Add a function, with arguments (of one type), to be called when the test is completed."""
            def call() -> None:
                function(*args)
            self._cleanups.append(call)

        def setUp(self) -> None:
            """Hook method for setting up the test fixture before exercising it."""
            pass

        def tearDown(self) -> None:
            """Hook method for deconstructing the test fixture after testing it."""
            pass

        @classmethod
        def setUpClass(cls) -> None:
            """Hook method for setting up class fixture before running tests in the class."""
            pass

        @classmethod
        def tearDownClass(cls) -> None:
            """Hook method for deconstructing the class fixture after running all tests in the class."""
            pass

        def runTest(self) -> None:
            raise AttributeError("'" + type(self).__name__ + "' object has no attribute 'runTest'")

        def countTestCases(self) -> int:
            return 1

        def defaultTestResult(self) -> TestResult:
            return TestResult()

        def shortDescription(self) -> str | None:
            doc = __mpy_ut_doc(self, self._testMethodName)
            return doc.strip().split("\n")[0].strip() if doc else None

        def id(self) -> str:
            return "%s.%s" % (__mpy_ut_class(self), self._testMethodName)

        def __str__(self) -> str:
            return "%s (%s.%s)" % (self._testMethodName, __mpy_ut_class(self), self._testMethodName)

        def __repr__(self) -> str:
            return "<%s testMethod=%s>" % (__mpy_ut_class(self), self._testMethodName)

        def subTest(self, msg: str | None = None, **params: _T) -> _SubTestContext:
            """A context manager: the enclosed block is a subtest identified by msg and params."""
            parent = self._subtest
            desc: list[str] = []
            if parent is not None:
                desc = list(parent._params)
            for k in params:
                desc.append("{}={!r}".format(k, params[k]))
            sub = _SubTest(self, msg, desc)
            self._subtest = sub
            return _SubTestContext(self, sub, parent)

        def _callTestMethod(self) -> None:
            if not __mpy_ut_call(self, self._testMethodName):
                if self._testMethodName == "runTest":
                    self.runTest()
                else:
                    raise AttributeError("'" + type(self).__name__ + "' object has no attribute '" +
                                         self._testMethodName + "'")

        def _part(self, outcome: _Outcome, what: int) -> None:
            """Run setUp (0), the test (1) or tearDown (2), its exception recorded in outcome."""
            old_success = outcome.success
            outcome.success = True
            self._failframe = ""
            self._failcontext = None
            try:
                if what == 0:
                    self.setUp()
                elif what == 1:
                    self._callTestMethod()
                else:
                    self.tearDown()
            except KeyboardInterrupt:
                raise
            except SkipTest as e:
                outcome.success = False
                outcome.result.addSkip(self, str(e))
            except _ShouldStop:
                pass
            except BaseException as e:
                self._record(outcome, e)
            outcome.success = outcome.success and old_success

        def _record(self, outcome: _Outcome, e: BaseException) -> None:
            if outcome.expecting_failure:
                outcome.expectedFailure = e
            else:
                outcome.success = False
                if isinstance(e, AssertionError):
                    outcome.result.addFailure(self, e)
                else:
                    outcome.result.addError(self, e)

        def run(self, result: TestResult | None = None) -> TestResult:
            stop_run = False
            if result is None:
                result = self.defaultTestResult()
                result.startTestRun()
                stop_run = True
            result.startTest(self)
            try:
                skip_why = __mpy_ut_skip(self, self._testMethodName)
                if skip_why is not None:
                    result.addSkip(self, skip_why)
                    return result
                expecting_failure = __mpy_ut_xfail(self, self._testMethodName)
                outcome = _Outcome(result)
                start_time = _time.perf_counter()
                self._outcome = outcome
                self._part(outcome, 0)
                if outcome.success:
                    outcome.expecting_failure = expecting_failure
                    self._part(outcome, 1)
                    outcome.expecting_failure = False
                    self._part(outcome, 2)
                self.doCleanups()
                result.addDuration(self, _time.perf_counter() - start_time)
                if outcome.success:
                    if expecting_failure:
                        fe = outcome.expectedFailure
                        if fe is not None:
                            result.addExpectedFailure(self, fe)
                        else:
                            result.addUnexpectedSuccess(self)
                    else:
                        result.addSuccess(self)
                self._outcome = None
                return result
            finally:
                result.stopTest(self)
                if stop_run:
                    result.stopTestRun()

        def doCleanups(self) -> bool:
            """Execute all cleanup functions. Normally called for you after tearDown."""
            outcome = self._outcome
            ok = True
            while self._cleanups:
                function = self._cleanups.pop()
                try:
                    function()
                except KeyboardInterrupt:
                    raise
                except BaseException as e:
                    ok = False
                    if outcome is not None:
                        old = outcome.expecting_failure
                        outcome.expecting_failure = False
                        self._record(outcome, e)
                        outcome.expecting_failure = old
            return ok

        def debug(self) -> None:
            """Run the test without collecting errors in a TestResult"""
            skip_why = __mpy_ut_skip(self, self._testMethodName)
            if skip_why is not None:
                raise SkipTest(skip_why)
            self.setUp()
            self._callTestMethod()
            self.tearDown()
            while self._cleanups:
                self._cleanups.pop()()

        def skipTest(self, reason: str) -> None:
            """Skip this test."""
            raise SkipTest(reason)

        def _fail(self, msg: str | None, at: str) -> None:
            self._failframe = at
            raise AssertionError(msg if msg is not None else "None")

        def fail(self, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Fail immediately, with the given message."""
            self._fail(msg, _at)

        def _formatMessage(self, msg: str | None, standardMsg: str) -> str:
            if not self.longMessage:
                return msg or standardMsg
            if msg is None:
                return standardMsg
            return '%s : %s' % (standardMsg, msg)

        def assertFalse(self, expr: _T, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Check that the expression is false."""
            if expr:
                self._fail(self._formatMessage(msg, "%s is not false" % safe_repr(expr)), _at)

        def assertTrue(self, expr: _T, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Check that the expression is true."""
            if not expr:
                self._fail(self._formatMessage(msg, "%s is not true" % safe_repr(expr)), _at)

        def assertRaises(self, expected_exception: type[BaseException], callable_obj=None, *args,
                         msg: str | None = None, _at: str = sys._site("frame")) -> _AssertRaisesContext:
            """Fail unless an exception of class expected_exception is raised by the callable (or in the
            with block of the context returned)."""
            context = _AssertRaisesContext(expected_exception, self, None, _at)
            context.msg = msg
            if callable_obj is not None:
                context.obj_name = callable_obj.__name__
                with context:
                    callable_obj(*args)
            return context

        def assertRaisesRegex(self, expected_exception: type[BaseException], expected_regex: str, callable_obj=None,
                              *args, msg: str | None = None, _at: str = sys._site("frame")) -> _AssertRaisesContext:
            """Asserts that the message in a raised exception matches a regex."""
            context = _AssertRaisesContext(expected_exception, self, expected_regex, _at)
            context.msg = msg
            if callable_obj is not None:
                context.obj_name = callable_obj.__name__
                with context:
                    callable_obj(*args)
            return context

        def _baseAssertEqual(self, first: _T, second: _S, msg: str | None, at: str) -> None:
            if not first == second:
                a, b = _common_shorten_repr(first, second)
                self._fail(self._formatMessage(msg, '%s != %s' % (a, b)), at)

        def assertEqual(self, first: _T, second: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Fail if the two objects are unequal as determined by the '==' operator."""
            if isinstance(first, str) and isinstance(second, str):
                self._multiLineEqual(first, second, msg, _at)
            elif isinstance(first, list) and isinstance(second, list):
                self._sequenceEqual(first, second, msg, "list", _at)
            elif isinstance(first, tuple) and isinstance(second, tuple):
                self._tupleEqual(first, second, msg, _at)
            elif isinstance(first, dict) and isinstance(second, dict):
                self._dictEqual(first, second, msg, _at)
            elif isinstance(first, frozenset) and isinstance(second, frozenset):
                self._setEqual(first, second, msg, _at)
            elif isinstance(first, set) and isinstance(second, set):
                self._setEqual(first, second, msg, _at)
            else:
                self._baseAssertEqual(first, second, msg, _at)

        def assertNotEqual(self, first: _T, second: _S, msg: str | None = None, *,
                           _at: str = sys._site("frame")) -> None:
            """Fail if the two objects are equal as determined by the '!=' operator."""
            if not first != second:
                self._fail(self._formatMessage(msg, '%s == %s' % (safe_repr(first), safe_repr(second))), _at)

        def assertAlmostEqual(self, first: _T, second: _S, places: int | None = None, msg: str | None = None,
                              delta: _V = None, *, _at: str = sys._site("frame")) -> None:
            """Fail if the two objects are unequal as determined by their difference rounded to the given
            number of decimal places (default 7), or by the difference being more than the given delta."""
            if first == second:
                return
            if delta is not None and places is not None:
                raise TypeError("specify delta or places not both")
            diff = abs(first - second)
            if delta is not None:
                if diff <= delta:
                    return
                standardMsg = '%s != %s within %s delta (%s difference)' % (
                    safe_repr(first), safe_repr(second), safe_repr(delta), safe_repr(diff))
            else:
                p = places if places is not None else 7
                if round(diff, p) == 0:
                    return
                standardMsg = '%s != %s within %r places (%s difference)' % (
                    safe_repr(first), safe_repr(second), p, safe_repr(diff))
            self._fail(self._formatMessage(msg, standardMsg), _at)

        def assertNotAlmostEqual(self, first: _T, second: _S, places: int | None = None, msg: str | None = None,
                                 delta: _V = None, *, _at: str = sys._site("frame")) -> None:
            """Fail if the two objects are equal as determined by their difference rounded to the given
            number of decimal places (default 7), or by the difference being less than the given delta."""
            if delta is not None and places is not None:
                raise TypeError("specify delta or places not both")
            diff = abs(first - second)
            if delta is not None:
                if not (first == second) and diff > delta:
                    return
                standardMsg = '%s == %s within %s delta (%s difference)' % (
                    safe_repr(first), safe_repr(second), safe_repr(delta), safe_repr(diff))
            else:
                p = places if places is not None else 7
                if not (first == second) and round(diff, p) != 0:
                    return
                standardMsg = '%s == %s within %r places' % (safe_repr(first), safe_repr(second), p)
            self._fail(self._formatMessage(msg, standardMsg), _at)

        def _truncateMessage(self, message: str, diff: str) -> str:
            max_diff = self.maxDiff
            if max_diff is None or len(diff) <= max_diff:
                return message + diff
            return message + ('\nDiff is %s characters long. Set self.maxDiff to None to see it.' % len(diff))

        def _seqDiffer(self, r1: list[str], r2: list[str], same: bool, full1: str, full2: str, pf1: str, pf2: str,
                       msg: str | None, seq_type_name: str, at: str) -> None:
            """(assertSequenceEqual, from the items' reprs: r1, r2)"""
            if same:
                return
            len1 = len(r1)
            len2 = len(r2)
            a, b = _shorten_reprs(full1, full2)
            differing = '%ss differ: %s != %s\n' % (seq_type_name.capitalize(), a, b)
            for i in range(min(len1, len2)):
                if r1[i] != r2[i]:
                    x, y = _shorten_reprs(r1[i], r2[i])
                    differing += '\nFirst differing element %d:\n%s\n%s\n' % (i, x, y)
                    break
            if len1 > len2:
                differing += '\nFirst %s contains %d additional elements.\n' % (seq_type_name, len1 - len2)
                differing += 'First extra element %d:\n%s\n' % (len2, r1[len2])
            elif len1 < len2:
                differing += '\nSecond %s contains %d additional elements.\n' % (seq_type_name, len2 - len1)
                differing += 'First extra element %d:\n%s\n' % (len1, r2[len1])
            diffMsg = '\n' + '\n'.join(_difflib.ndiff(pf1.splitlines(), pf2.splitlines()))
            self._fail(self._formatMessage(msg, self._truncateMessage(differing, diffMsg)), at)

        def _sequenceEqual(self, seq1: list[_T], seq2: list[_S], msg: str | None, seq_type_name: str, at: str) -> None:
            if seq1 == seq2:
                return
            self._seqDiffer([safe_repr(x) for x in seq1], [safe_repr(x) for x in seq2], False, safe_repr(seq1),
                            safe_repr(seq2), _pprint.pformat(seq1), _pprint.pformat(seq2), msg, seq_type_name, at)

        def _tupleEqual(self, tuple1: _T, tuple2: _S, msg: str | None, at: str) -> None:
            if tuple1 == tuple2:
                return
            self._seqDiffer([safe_repr(x) for x in tuple1], [safe_repr(x) for x in tuple2], False,
                            safe_repr(tuple1), safe_repr(tuple2), _pprint.pformat(tuple1), _pprint.pformat(tuple2),
                            msg, "tuple", at)

        def assertSequenceEqual(self, seq1: list[_T], seq2: list[_S], msg: str | None = None, *,
                                _at: str = sys._site("frame")) -> None:
            """An equality assertion for ordered sequences (like lists and tuples)."""
            self._sequenceEqual(seq1, seq2, msg, "sequence", _at)

        def assertListEqual(self, list1: list[_T], list2: list[_S], msg: str | None = None, *,
                            _at: str = sys._site("frame")) -> None:
            """A list-specific equality assertion."""
            self._sequenceEqual(list1, list2, msg, "list", _at)

        def assertTupleEqual(self, tuple1: _T, tuple2: _S, msg: str | None = None, *,
                             _at: str = sys._site("frame")) -> None:
            """A tuple-specific equality assertion."""
            self._tupleEqual(tuple1, tuple2, msg, _at)

        def _setEqual(self, set1: _T, set2: _S, msg: str | None, at: str) -> None:
            difference1 = set1.difference(set2)
            difference2 = set2.difference(set1)
            if not (difference1 or difference2):
                return
            lines: list[str] = []
            if difference1:
                lines.append('Items in the first set but not the second:')
                for item in difference1:
                    lines.append(repr(item))
            if difference2:
                lines.append('Items in the second set but not the first:')
                for item in difference2:
                    lines.append(repr(item))
            self._fail(self._formatMessage(msg, '\n'.join(lines)), at)

        def assertSetEqual(self, set1: _T, set2: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """A set-specific equality assertion."""
            self._setEqual(set1, set2, msg, _at)

        def assertIn(self, member: _T, container: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a in b), but with a nicer default message."""
            if member not in container:
                self._fail(self._formatMessage(msg, '%s not found in %s' % (safe_repr(member), safe_repr(container))),
                           _at)

        def assertNotIn(self, member: _T, container: _S, msg: str | None = None, *,
                        _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a not in b), but with a nicer default message."""
            if member in container:
                self._fail(self._formatMessage(msg, '%s unexpectedly found in %s' % (safe_repr(member),
                                                                                     safe_repr(container))), _at)

        def assertIs(self, expr1: _T, expr2: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a is b), but with a nicer default message."""
            if not _identical(expr1, expr2):
                self._fail(self._formatMessage(msg, '%s is not %s' % (safe_repr(expr1), safe_repr(expr2))), _at)

        def assertIsNot(self, expr1: _T, expr2: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a is not b), but with a nicer default message."""
            if _identical(expr1, expr2):
                self._fail(self._formatMessage(msg, 'unexpectedly identical: %s' % (safe_repr(expr1),)), _at)

        def _dictEqual(self, d1: dict[_K, _V], d2: dict[_T, _S], msg: str | None, at: str) -> None:
            if d1 != d2:
                a, b = _common_shorten_repr(d1, d2)
                diff = '\n' + '\n'.join(_difflib.ndiff(_pprint.pformat(d1).splitlines(), _pprint.pformat(d2).splitlines()))
                self._fail(self._formatMessage(msg, self._truncateMessage('%s != %s' % (a, b), diff)), at)

        def assertDictEqual(self, d1: dict[_K, _V], d2: dict[_T, _S], msg: str | None = None, *,
                            _at: str = sys._site("frame")) -> None:
            self._dictEqual(d1, d2, msg, _at)

        def assertCountEqual(self, first: _T, second: _S, msg: str | None = None, *,
                             _at: str = sys._site("frame")) -> None:
            """Asserts that two iterables have the same elements, the same number of times, without regard
            to order."""
            first_seq = list(first)
            second_seq = list(second)
            s: dict[str, int] = {}
            t: dict[str, int] = {}
            for x in first_seq:
                k = repr(x)
                s[k] = s.get(k, 0) + 1
            for y in second_seq:
                k = repr(y)
                t[k] = t.get(k, 0) + 1
            lines: list[str] = []
            for k in s:
                if s[k] != t.get(k, 0):
                    lines.append('First has %d, Second has %d:  %s' % (s[k], t.get(k, 0), k))
            for k in t:
                if k not in s:
                    lines.append('First has %d, Second has %d:  %s' % (0, t[k], k))
            if lines:
                standardMsg = self._truncateMessage('Element counts were not equal:\n', '\n'.join(lines))
                self._fail(self._formatMessage(msg, standardMsg), _at)

        def _multiLineEqual(self, first: str, second: str, msg: str | None, at: str) -> None:
            if first != second:
                if len(first) > self._diffThreshold or len(second) > self._diffThreshold:
                    self._baseAssertEqual(first, second, msg, at)
                first_presplit = first
                second_presplit = second
                if first and second:
                    if first[-1] != '\n' or second[-1] != '\n':
                        first_presplit += '\n'
                        second_presplit += '\n'
                elif second and second[-1] != '\n':
                    second_presplit += '\n'
                elif first and first[-1] != '\n':
                    first_presplit += '\n'
                firstlines = first_presplit.splitlines(keepends=True)
                secondlines = second_presplit.splitlines(keepends=True)
                a, b = _common_shorten_repr(first, second)
                diff = '\n' + ''.join(_difflib.ndiff(firstlines, secondlines))
                self._fail(self._formatMessage(msg, self._truncateMessage('%s != %s' % (a, b), diff)), at)

        def assertMultiLineEqual(self, first: str, second: str, msg: str | None = None, *,
                                 _at: str = sys._site("frame")) -> None:
            """Assert that two multi-line strings are equal."""
            self._multiLineEqual(first, second, msg, _at)

        def assertLess(self, a: _T, b: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a < b), but with a nicer default message."""
            if not a < b:
                self._fail(self._formatMessage(msg, '%s not less than %s' % (safe_repr(a), safe_repr(b))), _at)

        def assertLessEqual(self, a: _T, b: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a <= b), but with a nicer default message."""
            if not a <= b:
                self._fail(self._formatMessage(msg, '%s not less than or equal to %s' % (safe_repr(a), safe_repr(b))),
                           _at)

        def assertGreater(self, a: _T, b: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a > b), but with a nicer default message."""
            if not a > b:
                self._fail(self._formatMessage(msg, '%s not greater than %s' % (safe_repr(a), safe_repr(b))), _at)

        def assertGreaterEqual(self, a: _T, b: _S, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Just like self.assertTrue(a >= b), but with a nicer default message."""
            if not a >= b:
                self._fail(self._formatMessage(msg, '%s not greater than or equal to %s' % (safe_repr(a),
                                                                                             safe_repr(b))), _at)

        def assertIsNone(self, obj: _T, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Same as self.assertTrue(obj is None), with a nicer default message."""
            if obj is not None:
                self._fail(self._formatMessage(msg, '%s is not None' % (safe_repr(obj),)), _at)

        def assertIsNotNone(self, obj: _T, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            """Included for symmetry with assertIsNone."""
            if obj is None:
                self._fail(self._formatMessage(msg, 'unexpectedly None'), _at)

        def _isInstance(self, ok: bool, obj: _T, cls_repr: str, msg: str | None, *, _at: str = sys._site("frame")) -> None:
            """(self.assertIsInstance(obj, C, msg), as the compiler writes it)"""
            if not ok:
                self._fail(self._formatMessage(msg, '%s is not an instance of %s' % (safe_repr(obj), cls_repr)), _at)

        def _notIsInstance(self, ok: bool, obj: _T, cls_repr: str, msg: str | None, *,
                           _at: str = sys._site("frame")) -> None:
            """(self.assertNotIsInstance(obj, C, msg), as the compiler writes it)"""
            if ok:
                self._fail(self._formatMessage(msg, '%s is an instance of %s' % (safe_repr(obj), cls_repr)), _at)

        def assertRegex(self, text: str, expected_regex: str, msg: str | None = None, *,
                        _at: str = sys._site("frame")) -> None:
            """Fail the test unless the text matches the regular expression."""
            if not _re.search(expected_regex, text):
                standardMsg = "Regex didn't match: %r not found in %r" % (expected_regex, text)
                self._fail(self._formatMessage(msg, standardMsg), _at)

        def assertNotRegex(self, text: str, unexpected_regex: str, msg: str | None = None, *,
                           _at: str = sys._site("frame")) -> None:
            """Fail the test if the text matches the regular expression."""
            match = _re.search(unexpected_regex, text)
            if match:
                standardMsg = 'Regex matched: %r matches %r in %r' % (text[match.start():match.end()],
                                                                      unexpected_regex, text)
                self._fail(self._formatMessage(msg, standardMsg), _at)

        def assertStartsWith(self, s: str, prefix: str, msg: str | None = None, *,
                             _at: str = sys._site("frame")) -> None:
            if s.startswith(prefix):
                return
            self._fail(self._formatMessage(msg, "%s doesn't start with %s" % (safe_repr(s, short=True),
                                                                               safe_repr(prefix))), _at)

        def assertNotStartsWith(self, s: str, prefix: str, msg: str | None = None, *,
                                _at: str = sys._site("frame")) -> None:
            if not s.startswith(prefix):
                return
            self._fail(self._formatMessage(msg, "%s starts with %s" % (safe_repr(s, short=True), safe_repr(prefix))),
                       _at)

        def assertEndsWith(self, s: str, suffix: str, msg: str | None = None, *, _at: str = sys._site("frame")) -> None:
            if s.endswith(suffix):
                return
            self._fail(self._formatMessage(msg, "%s doesn't end with %s" % (safe_repr(s, short=True),
                                                                             safe_repr(suffix))), _at)

        def assertNotEndsWith(self, s: str, suffix: str, msg: str | None = None, *,
                              _at: str = sys._site("frame")) -> None:
            if not s.endswith(suffix):
                return
            self._fail(self._formatMessage(msg, "%s ends with %s" % (safe_repr(s, short=True), safe_repr(suffix))),
                       _at)

    def _identical(a: _T, b: _S) -> bool:
        """a is b (values: the same type and value)"""
        if a is None or b is None:
            return a is None and b is None
        if isinstance(a, (bool, int, float, str, bytes)) or isinstance(b, (bool, int, float, str, bytes)):
            return type(a).__name__ == type(b).__name__ and repr(a) == repr(b)
        return a is b

    class _SubTest(TestCase):
        def __init__(self, test_case: TestCase, message: str | None, params: list[str]) -> None:
            super().__init__()
            self._message = message
            self.test_case = test_case
            self._params = params

        def runTest(self) -> None:
            raise NotImplementedError("subtests cannot be run directly")

        def _subDescription(self) -> str:
            parts: list[str] = []
            if self._message is not None:
                parts.append("[{}]".format(self._message))
            if self._params:
                parts.append("({})".format(', '.join(self._params)))
            return " ".join(parts) or '(<subtest>)'

        def id(self) -> str:
            return "{} {}".format(self.test_case.id(), self._subDescription())

        def shortDescription(self) -> str | None:
            return self.test_case.shortDescription()

        def __str__(self) -> str:
            return "{} {}".format(self.test_case, self._subDescription())

    class _ErrorHolder(TestCase):
        """Placeholder for a TestCase inside a result: an error outside the tests (setUpClass ...)."""

        def __init__(self, description: str) -> None:
            super().__init__()
            self.description = description

        def id(self) -> str:
            return self.description

        def shortDescription(self) -> str | None:
            return None

        def __repr__(self) -> str:
            return "<ErrorHolder description=%r>" % (self.description,)

        def __str__(self) -> str:
            return self.description

    class FunctionTestCase(TestCase):
        """A test case that wraps a test function (and optional setUp / tearDown functions)."""

        def __init__(self, testFunc: Callable[[], None], setUp: Callable[[], None] | None = None,
                     tearDown: Callable[[], None] | None = None, description: str | None = None) -> None:
            super().__init__()
            self._setUpFunc = setUp
            self._tearDownFunc = tearDown
            self._testFunc = testFunc
            self._description = description

        def setUp(self) -> None:
            f = self._setUpFunc
            if f is not None:
                f()

        def tearDown(self) -> None:
            f = self._tearDownFunc
            if f is not None:
                f()

        def runTest(self) -> None:
            self._testFunc()

        def id(self) -> str:
            return self._testFunc.__name__

        def __str__(self) -> str:
            return "unittest.case.FunctionTestCase (%s)" % (self._testFunc.__name__,)

        def __repr__(self) -> str:
            return "<unittest.case.FunctionTestCase tec=%s>" % (self._testFunc.__name__,)

        def shortDescription(self) -> str | None:
            return self._description

    # ---- suites

    class TestSuite(_TestItem):
        """A test suite is a composite test consisting of a number of TestCases."""

        def __init__(self, tests: _T = None) -> None:
            self._tests: list[_TestItem] = []
            if tests is not None:
                for t in tests:
                    self.addTest(t)

        def __repr__(self) -> str:
            return "<unittest.suite.TestSuite tests=%s>" % (repr([str(t) for t in self._tests]),)

        def __iter__(self):
            for t in self._tests:
                yield t

        def countTestCases(self) -> int:
            cases = 0
            for test in self._tests:
                cases += test.countTestCases()
            return cases

        def addTest(self, test: _TestItem) -> None:
            self._tests.append(test)

        def addTests(self, tests: _T) -> None:
            for test in tests:
                self.addTest(test)

        def run(self, result: TestResult) -> TestResult:
            topLevel = False
            if not result._testRunEntered:
                result._testRunEntered = topLevel = True
            for test in self._tests:
                if result.shouldStop:
                    break
                if isinstance(test, TestCase):
                    self._tearDownPreviousClass(test, result)
                    self._handleModuleFixture(test, result)
                    self._handleClassSetUp(test, result)
                    result._previousTest = test
                    if result._classSetupFailed.get(__mpy_ut_class(test), False) or result._moduleSetUpFailed:
                        continue
                test.run(result)
            if topLevel:
                self._tearDownPreviousClass(None, result)
                self._handleModuleTearDown(result)
                result._testRunEntered = False
            return result

        def debug(self) -> None:
            for test in self._tests:
                if isinstance(test, TestCase):
                    test.debug()

        def _addClassOrModuleLevelException(self, result: TestResult, e: BaseException, errorName: str) -> None:
            error = _ErrorHolder(errorName)
            if isinstance(e, SkipTest):
                result.addSkip(error, str(e))
            else:
                result.addError(error, e)

        def _handleClassSetUp(self, test: TestCase, result: TestResult) -> None:
            previous = result._previousTest
            currentClass = __mpy_ut_class(test)
            if previous is not None and __mpy_ut_class(previous) == currentClass:
                return
            if result._moduleSetUpFailed:
                return
            if __mpy_ut_skip(test, "") is not None:
                return
            result._classSetupFailed[currentClass] = False
            try:
                __mpy_ut_fixture(test, 0)
            except Exception as e:
                result._classSetupFailed[currentClass] = True
                self._addClassOrModuleLevelException(result, e, 'setUpClass (%s)' % (currentClass,))

        def _tearDownPreviousClass(self, test: TestCase | None, result: TestResult) -> None:
            previous = result._previousTest
            if previous is None:
                return
            previousClass = __mpy_ut_class(previous)
            if test is not None and __mpy_ut_class(test) == previousClass:
                return
            if result._classSetupFailed.get(previousClass, False) or result._moduleSetUpFailed:
                return
            if __mpy_ut_skip(previous, "") is not None:
                return
            try:
                __mpy_ut_fixture(previous, 1)
            except Exception as e:
                self._addClassOrModuleLevelException(result, e, 'tearDownClass (%s)' % (previousClass,))

        def _handleModuleFixture(self, test: TestCase, result: TestResult) -> None:
            previous = result._previousTest
            currentModule = __mpy_ut_module(test)
            if previous is not None and __mpy_ut_module(previous) == currentModule:
                return
            self._handleModuleTearDown(result)
            result._moduleSetUpFailed = False
            try:
                __mpy_ut_modfix(currentModule, True)
            except Exception as e:
                result._moduleSetUpFailed = True
                self._addClassOrModuleLevelException(result, e, 'setUpModule (%s)' % (currentModule,))

        def _handleModuleTearDown(self, result: TestResult) -> None:
            previous = result._previousTest
            if previous is None or result._moduleSetUpFailed:
                return
            previousModule = __mpy_ut_module(previous)
            try:
                __mpy_ut_modfix(previousModule, False)
            except Exception as e:
                self._addClassOrModuleLevelException(result, e, 'tearDownModule (%s)' % (previousModule,))

    BaseTestSuite = TestSuite

    # ---- loading

    def _fnmatch(name: str, pattern: str) -> bool:
        import fnmatch
        return fnmatch.fnmatchcase(name, pattern)

    class TestLoader:
        """Makes the test suites of the program's test cases (found when it was compiled)."""
        testMethodPrefix = 'test'

        def __init__(self) -> None:
            self.errors: list[str] = []
            self.testNamePatterns: list[str] | None = None

        def _included(self, t: TestCase) -> bool:
            pats = self.testNamePatterns
            if pats is None:
                return True
            full = t.id()
            for pat in pats:
                if _fnmatch(full, pat):
                    return True
            return False

        def loadTestsFromTestCase(self, testCaseClass: type[TestCase]) -> TestSuite:
            """Return a suite of all test cases contained in testCaseClass"""
            suite = TestSuite()
            for t in __mpy_ut_tests(False):
                if type(t) is testCaseClass and self._included(t):
                    suite.addTest(t)
            return suite

        def getTestCaseNames(self, testCaseClass: type[TestCase]) -> list[str]:
            """Return a sorted sequence of method names found within testCaseClass"""
            return [t._testMethodName for t in __mpy_ut_tests(False) if type(t) is testCaseClass and self._included(t)]

        def _main_tests(self) -> TestSuite:
            """(the test cases of the main module, by class)"""
            suite = TestSuite()
            current: TestSuite | None = None
            cls = ""
            for t in __mpy_ut_tests(True):
                if not self._included(t):
                    continue
                if current is None or __mpy_ut_class(t) != cls:
                    current = TestSuite()
                    cls = __mpy_ut_class(t)
                    suite.addTest(current)
                current.addTest(t)
            return suite

        def loadTestsFromName(self, name: str) -> TestSuite:
            """The tests named name: a class of the main module, or Class.test_method."""
            suite = TestSuite()
            for t in __mpy_ut_tests(False):
                ident = t.id()
                for prefix in ("__main__." + name, name):
                    if ident == prefix or (ident.startswith(prefix + ".") and self._included(t)):
                        suite.addTest(t)
                        break
            if not suite._tests:
                raise AttributeError("module '__main__' has no attribute '" + name.split(".")[0] + "'")
            return suite

        def loadTestsFromNames(self, names: list[str]) -> TestSuite:
            suites = TestSuite()
            for name in names:
                suites.addTest(self.loadTestsFromName(name))
            return suites

        def discover(self, start_dir: str, pattern: str = 'test*.py', top_level_dir: str | None = None) -> TestSuite:
            raise NotImplementedError("test discovery is not available in compiled programs")

    defaultTestLoader = TestLoader()

    # ---- running

    def _stderr_write(s: str) -> int:
        return sys.stderr.write(s)

    def _stderr_flush() -> None:
        sys.stderr.flush()

    class _WritelnDecorator:
        """A stream's write (and flush), with a handy writeln."""

        def __init__(self, write: Callable[[str], int], flush: Callable[[], None]) -> None:
            self._write = write
            self._flush = flush

        def write(self, s: str) -> int:
            return self._write(s)

        def flush(self) -> None:
            self._flush()

        def writeln(self, arg: str | None = None) -> None:
            if arg:
                self._write(arg)
            self._write('\n')

    class TextTestResult(TestResult):
        """A test result class that can print formatted text results to a stream."""
        separator1 = '=' * 70
        separator2 = '-' * 70

        def __init__(self, stream: _WritelnDecorator, descriptions: bool, verbosity: int, *,
                     durations: int | None = None) -> None:
            super().__init__()
            self.stream = stream
            self.showAll = verbosity > 1
            self.dots = verbosity == 1
            self.descriptions = descriptions
            self._newline = True
            self.durations = durations

        def getDescription(self, test: TestCase) -> str:
            doc_first_line = test.shortDescription()
            if self.descriptions and doc_first_line:
                return str(test) + '\n' + doc_first_line
            return str(test)

        def startTest(self, test: TestCase) -> None:
            super().startTest(test)
            if self.showAll:
                self.stream.write(self.getDescription(test))
                self.stream.write(" ... ")
                self.stream.flush()
                self._newline = False

        def _write_status(self, test: TestCase, status: str) -> None:
            is_subtest = isinstance(test, _SubTest)
            if is_subtest or self._newline:
                if not self._newline:
                    self.stream.writeln()
                if is_subtest:
                    self.stream.write("  ")
                self.stream.write(self.getDescription(test))
                self.stream.write(" ... ")
            self.stream.writeln(status)
            self.stream.flush()
            self._newline = True

        def addSubTest(self, test: TestCase, subtest: TestCase, err: BaseException | None) -> None:
            if err is not None:
                if self.showAll:
                    self._write_status(subtest, "FAIL" if isinstance(err, AssertionError) else "ERROR")
                elif self.dots:
                    self.stream.write("F" if isinstance(err, AssertionError) else "E")
                    self.stream.flush()
            super().addSubTest(test, subtest, err)

        def addSuccess(self, test: TestCase) -> None:
            super().addSuccess(test)
            if self.showAll:
                self._write_status(test, "ok")
            elif self.dots:
                self.stream.write(".")
                self.stream.flush()

        def addError(self, test: TestCase, err: BaseException) -> None:
            super().addError(test, err)
            if self.showAll:
                self._write_status(test, "ERROR")
            elif self.dots:
                self.stream.write("E")
                self.stream.flush()

        def addFailure(self, test: TestCase, err: BaseException) -> None:
            super().addFailure(test, err)
            if self.showAll:
                self._write_status(test, "FAIL")
            elif self.dots:
                self.stream.write("F")
                self.stream.flush()

        def addSkip(self, test: TestCase, reason: str) -> None:
            super().addSkip(test, reason)
            if self.showAll:
                self._write_status(test, "skipped {0!r}".format(reason))
            elif self.dots:
                self.stream.write("s")
                self.stream.flush()

        def addExpectedFailure(self, test: TestCase, err: BaseException) -> None:
            super().addExpectedFailure(test, err)
            if self.showAll:
                self.stream.writeln("expected failure")
                self.stream.flush()
            elif self.dots:
                self.stream.write("x")
                self.stream.flush()

        def addUnexpectedSuccess(self, test: TestCase) -> None:
            super().addUnexpectedSuccess(test)
            if self.showAll:
                self.stream.writeln("unexpected success")
                self.stream.flush()
            elif self.dots:
                self.stream.write("u")
                self.stream.flush()

        def printErrors(self) -> None:
            if self.dots or self.showAll:
                self.stream.writeln()
                self.stream.flush()
            self.printErrorList("ERROR", self.errors)
            self.printErrorList("FAIL", self.failures)
            if self.unexpectedSuccesses:
                self.stream.writeln(self.separator1)
                for test in self.unexpectedSuccesses:
                    self.stream.writeln("UNEXPECTED SUCCESS: " + self.getDescription(test))
                self.stream.flush()

        def printErrorList(self, flavour: str, errors: list[tuple[TestCase, str]]) -> None:
            for test, err in errors:
                self.stream.writeln(self.separator1)
                self.stream.writeln("%s: %s" % (flavour, self.getDescription(test)))
                self.stream.writeln(self.separator2)
                self.stream.writeln("%s" % err)
                self.stream.flush()

    class TextTestRunner:
        """A test runner class that displays results in textual form (on stream: sys.stderr by default)."""

        def __init__(self, stream: _W = None, descriptions: bool = True, verbosity: int = 1, failfast: bool = False,
                     buffer: bool = False, resultclass: _R = None, warnings: str | None = None, *,
                     tb_locals: bool = False, durations: int | None = None) -> None:
            if stream is None:
                self.stream = _WritelnDecorator(_stderr_write, _stderr_flush)
            else:
                self.stream = _WritelnDecorator(stream.write, stream.flush)
            self.descriptions = descriptions
            self.verbosity = verbosity
            self.failfast = failfast
            self.buffer = buffer
            self.tb_locals = tb_locals
            self.durations = durations
            self.warnings = warnings

        def _makeResult(self) -> TextTestResult:
            return TextTestResult(self.stream, self.descriptions, self.verbosity, durations=self.durations)

        def _printDurations(self, result: TextTestResult) -> None:
            if not result.collectedDurations:
                return
            ls = sorted(result.collectedDurations, key=lambda x: x[1], reverse=True)
            d = self.durations
            if d is not None and d > 0:
                ls = ls[:d]
            self.stream.writeln("Slowest test durations")
            self.stream.writeln(result.separator2)
            hidden = False
            for test, elapsed in ls:
                if self.verbosity < 2 and elapsed < 0.001:
                    hidden = True
                    continue
                self.stream.writeln("%-10s %s" % ("%.3fs" % elapsed, test))
            if hidden:
                self.stream.writeln("\n(durations < 0.001s were hidden; use -v to show these durations)")
            else:
                self.stream.writeln("")

        def run(self, test: _TestItem) -> TextTestResult:
            """Run the given test case or test suite."""
            result = self._makeResult()
            result.failfast = self.failfast
            result.buffer = self.buffer
            result.tb_locals = self.tb_locals
            start_time = _time.perf_counter()
            result.startTestRun()
            try:
                test.run(result)
            finally:
                result.stopTestRun()
            stop_time = _time.perf_counter()
            time_taken = stop_time - start_time
            result.printErrors()
            if self.durations is not None:
                self._printDurations(result)
            self.stream.writeln(result.separator2)
            run = result.testsRun
            self.stream.writeln("Ran %d test%s in %.3fs" % (run, "s" if run != 1 else "", time_taken))
            self.stream.writeln()
            expected_fails = len(result.expectedFailures)
            unexpected_successes = len(result.unexpectedSuccesses)
            skipped = len(result.skipped)
            infos: list[str] = []
            if not result.wasSuccessful():
                self.stream.write("FAILED")
                failed = len(result.failures)
                errored = len(result.errors)
                if failed:
                    infos.append("failures=%d" % failed)
                if errored:
                    infos.append("errors=%d" % errored)
            elif run == 0 and not skipped:
                self.stream.write("NO TESTS RAN")
            else:
                self.stream.write("OK")
            if skipped:
                infos.append("skipped=%d" % skipped)
            if expected_fails:
                infos.append("expected failures=%d" % expected_fails)
            if unexpected_successes:
                infos.append("unexpected successes=%d" % unexpected_successes)
            if infos:
                self.stream.writeln(" (%s)" % (", ".join(infos),))
            else:
                self.stream.write("\n")
            self.stream.flush()
            return result

    class TestProgram:
        """A command-line program that runs a set of tests (those of the main module)."""

        def __init__(self, module: str = '__main__', defaultTest: str | None = None, argv: list[str] | None = None,
                     testRunner: _R = None, testLoader: TestLoader | None = None, exit: bool = True,
                     verbosity: int = 1, failfast: bool = False, catchbreak: bool = False, buffer: bool = False,
                     warnings: str | None = None, *, tb_locals: bool = False, durations: int | None = None) -> None:
            self.exit = exit
            self.failfast = failfast
            self.catchbreak = catchbreak
            self.verbosity = verbosity
            self.buffer = buffer
            self.tb_locals = tb_locals
            self.durations = durations
            self.warnings = warnings
            self.defaultTest = defaultTest
            self.testLoader = testLoader if testLoader is not None else defaultTestLoader
            args = argv if argv is not None else sys.argv
            self.progName = args[0].split("/")[-1] if args else "python -m unittest"
            self.testNames: list[str] = []
            self.testNamePatterns: list[str] = []
            self.parseArgs(args)
            self.test = self.createTests()
            self.result: TextTestResult | None = None
            if testRunner is None:
                self.result = TextTestRunner(verbosity=self.verbosity, failfast=self.failfast, buffer=self.buffer,
                                             warnings=self.warnings, tb_locals=self.tb_locals,
                                             durations=self.durations).run(self.test)
            else:
                self.result = testRunner.run(self.test)
            self._exit()

        def _exit(self) -> None:
            r = self.result
            if self.exit and r is not None:
                if not r.wasSuccessful():
                    sys.exit(1)
                elif r.testsRun == 0 and len(r.skipped) == 0:
                    sys.exit(_NO_TESTS_EXITCODE)
                else:
                    sys.exit(0)

        def usageExit(self, msg: str | None = None) -> None:
            if msg:
                print(msg)
            print("usage: %s [-h] [-v] [-q] [--locals] [--durations N] [-f] [-c] [-b] [-k TESTNAMEPATTERNS] "
                  "[tests ...]" % self.progName)
            sys.exit(2)

        def parseArgs(self, argv: list[str]) -> None:
            i = 1
            while i < len(argv):
                a = argv[i]
                if a in ("-h", "--help"):
                    self.usageExit()
                elif a in ("-v", "--verbose"):
                    self.verbosity = 2
                elif a in ("-q", "--quiet"):
                    self.verbosity = 0
                elif a in ("-f", "--failfast"):
                    self.failfast = True
                elif a in ("-c", "--catch"):
                    self.catchbreak = True
                elif a in ("-b", "--buffer"):
                    self.buffer = True
                elif a == "--locals":
                    self.tb_locals = True
                elif a == "--durations" and i + 1 < len(argv):
                    i += 1
                    self.durations = int(argv[i])
                elif a == "-k" and i + 1 < len(argv):
                    i += 1
                    p = argv[i]
                    self.testNamePatterns.append(p if "*" in p else "*%s*" % p)
                elif a.startswith("-k") and len(a) > 2:
                    p = a[2:]
                    self.testNamePatterns.append(p if "*" in p else "*%s*" % p)
                elif a.startswith("-") and a != "-":
                    self.usageExit("%s: error: unrecognized arguments: %s" % (self.progName, a))
                else:
                    self.testNames.append(a)
                i += 1
            if not self.testNames and self.defaultTest is not None:
                self.testNames = [self.defaultTest]

        def createTests(self) -> TestSuite:
            if self.testNamePatterns:
                self.testLoader.testNamePatterns = self.testNamePatterns
            if not self.testNames:
                return self.testLoader._main_tests()
            return self.testLoader.loadTestsFromNames(self.testNames)

    main = TestProgram

    def installHandler() -> None:
        pass

    def removeHandler(method: _T = None) -> None:
        pass

    def registerResult(result: TestResult) -> None:
        pass

    def removeResult(result: TestResult) -> bool:
        return False
