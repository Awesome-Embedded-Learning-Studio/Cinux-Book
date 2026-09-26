"""Minimal Test Framework

A lightweight test framework without external dependencies.
Inspired by unittest but simplified.
"""

from __future__ import annotations

import sys
import traceback
from dataclasses import dataclass, field
from typing import Any, Callable, List, Optional


# ============================================================
# ===================== EXCEPTIONS =========================
# ============================================================

class AssertionError(Exception):
    """Assertion failed."""
    pass


# ============================================================
# ===================== ASSERTIONS =========================
# ============================================================

def assert_equal(a: Any, b: Any, msg: str = "") -> None:
    """Assert that two values are equal."""
    if a != b:
        raise AssertionError(f"{msg}\nExpected: {a!r}\nGot: {b!r}")


def assert_not_equal(a: Any, b: Any, msg: str = "") -> None:
    """Assert that two values are not equal."""
    if a == b:
        raise AssertionError(f"{msg}\nExpected: {a!r} to not equal {b!r}")


def assert_true(value: Any, msg: str = "") -> None:
    """Assert that a value is truthy."""
    if not value:
        raise AssertionError(f"{msg}\nExpected truthy value, got {value!r}")


def assert_false(value: Any, msg: str = "") -> None:
    """Assert that a value is falsy."""
    if value:
        raise AssertionError(f"{msg}\nExpected falsy value, got {value!r}")


def assert_in(item: Any, container: Any, msg: str = "") -> None:
    """Assert that item is in container."""
    if item not in container:
        raise AssertionError(f"{msg}\nExpected {item!r} to be in {container!r}")


def assert_not_in(item: Any, container: Any, msg: str = "") -> None:
    """Assert that item is not in container."""
    if item in container:
        raise AssertionError(f"{msg}\nExpected {item!r} to not be in {container!r}")


def assert_raises(exc_type: type, func: Callable, *args, **kwargs) -> Exception:
    """Assert that calling func raises exc_type."""
    try:
        func(*args, **kwargs)
    except exc_type as e:
        return e
    except Exception as e:
        raise AssertionError(
            f"Expected {exc_type.__name__} but got {type(e).__name__}: {e}"
        )
    raise AssertionError(f"Expected {exc_type.__name__} but no exception was raised")


def assert_greater(a: Any, b: Any, msg: str = "") -> None:
    """Assert that a > b."""
    if not a > b:
        raise AssertionError(f"{msg}\nExpected {a!r} > {b!r}")


def assert_less(a: Any, b: Any, msg: str = "") -> None:
    """Assert that a < b."""
    if not a < b:
        raise AssertionError(f"{msg}\nExpected {a!r} < {b!r}")


def assert_len(collection: Any, expected: int, msg: str = "") -> None:
    """Assert that collection has expected length."""
    actual = len(collection)
    if actual != expected:
        raise AssertionError(f"{msg}\nExpected length {expected}, got {actual}")


# ============================================================
# ==================== TEST RESULTS =========================
# ============================================================

@dataclass
class TestResult:
    """Result of a single test execution."""
    name: str
    passed: bool
    error: Optional[str] = None
    duration_ms: float = 0.0


@dataclass
class SuiteResult:
    """Result of a test suite execution."""
    suite_name: str
    tests: List[TestResult] = field(default_factory=list)
    total_tests: int = 0
    passed: int = 0
    failed: int = 0
    total_duration_ms: float = 0.0

    @property
    def failed_tests(self) -> List[TestResult]:
        return [t for t in self.tests if not t.passed]


# ============================================================
# ===================== TEST CASE ==========================
# ============================================================

class TestCase:
    """Base class for test cases.

    Subclass this and define methods starting with 'test_'.
    """

    def setUp(self) -> None:
        """Hook called before each test method."""
        pass

    def tearDown(self) -> None:
        """Hook called after each test method."""
        pass

    def run_test(self, test_method: str) -> TestResult:
        """Run a single test method and return the result."""
        import time
        start = time.perf_counter()

        try:
            self.setUp()
            getattr(self, test_method)()
            self.tearDown()
            passed = True
            error = None
        except Exception as e:
            passed = False
            error = "".join(traceback.format_exception(type(e), e, e.__traceback__))
            # Try to run tearDown even if test failed
            try:
                self.tearDown()
            except Exception:
                pass

        duration = (time.perf_counter() - start) * 1000
        return TestResult(
            name=f"{self.__class__.__name__}.{test_method}",
            passed=passed,
            error=error,
            duration_ms=duration,
        )

    def list_tests(self) -> List[str]:
        """Return list of test method names."""
        return [m for m in dir(self) if m.startswith("test_")]


# ============================================================
# ===================== TEST SUITE =========================
# ============================================================

class TestSuite:
    """Collection of test cases."""

    def __init__(self, name: str = ""):
        self.name = name or "TestSuite"
        self._tests: List[TestCase] = []

    def add_test(self, test: TestCase) -> None:
        """Add a test case to the suite."""
        self._tests.append(test)

    def add_tests(self, tests: List[TestCase]) -> None:
        """Add multiple test cases to the suite."""
        self._tests.extend(tests)

    def run(self) -> SuiteResult:
        """Run all tests and return results."""
        import time
        start = time.perf_counter()

        results = []
        for test in self._tests:
            for test_method in test.list_tests():
                result = test.run_test(test_method)
                results.append(result)

        duration = (time.perf_counter() - start) * 1000

        suite_result = SuiteResult(
            suite_name=self.name,
            tests=results,
            total_tests=len(results),
            passed=sum(1 for r in results if r.passed),
            failed=sum(1 for r in results if not r.passed),
            total_duration_ms=duration,
        )
        return suite_result


# ============================================================
# ==================== TEST RUNNER =========================
# ============================================================

def print_result(result: SuiteResult, verbose: bool = False) -> None:
    """Print test results to stdout."""
    print(f"\n{'='*60}")
    print(f"Test Suite: {result.suite_name}")
    print(f"{'='*60}")

    for test in result.tests:
        if test.passed:
            print(f"  PASS {test.name} ({test.duration_ms:.2f}ms)")
        else:
            print(f"  FAIL {test.name}")
            if test.error:
                for line in test.error.splitlines():
                    print(f"    {line}")

    print(f"\n{'='*60}")
    print(f"Results: {result.passed}/{result.total_tests} passed")
    if result.failed > 0:
        print(f"  FAILED: {result.failed}")
    print(f"Duration: {result.total_duration_ms:.2f}ms")
    print(f"{'='*60}\n")


def run_test_suite(suite: TestSuite, verbose: bool = False) -> SuiteResult:
    """Run a test suite and print results."""
    result = suite.run()
    print_result(result, verbose)
    return result


def run_all_tests(suites: List[TestSuite], verbose: bool = False) -> List[SuiteResult]:
    """Run multiple test suites."""
    all_results = []

    print(f"\n{'#'*60}")
    print(f"# Running {len(suites)} test suite(s)")
    print(f"{'#'*60}\n")

    for suite in suites:
        result = suite.run()
        all_results.append(result)
        print_result(result, verbose)

    # Print summary
    total_tests = sum(r.total_tests for r in all_results)
    total_passed = sum(r.passed for r in all_results)
    total_failed = sum(r.failed for r in all_results)
    total_duration = sum(r.total_duration_ms for r in all_results)

    print(f"\n{'#'*60}")
    print(f"# OVERALL SUMMARY")
    print(f"{'#'*60}")
    print(f"Total: {total_passed}/{total_tests} passed")
    if total_failed > 0:
        print(f"  FAILED: {total_failed}")
    print(f"Duration: {total_duration:.2f}ms")
    print(f"{'#'*60}\n")

    return all_results
