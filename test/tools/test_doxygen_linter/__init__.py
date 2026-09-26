"""Doxygen Linter Test Package

Minimal test framework without external dependencies.
"""

from .test_framework import (
    TestCase,
    TestSuite,
    assert_equal,
    assert_true,
    assert_false,
    assert_not_equal,
    assert_in,
    assert_not_in,
    assert_raises,
    run_test_suite,
)

__all__ = [
    "TestCase",
    "TestSuite",
    "assert_equal",
    "assert_true",
    "assert_false",
    "assert_not_equal",
    "assert_in",
    "assert_not_in",
    "assert_raises",
    "run_test_suite",
]
