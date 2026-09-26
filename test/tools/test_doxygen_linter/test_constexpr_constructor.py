"""Tests for constexpr constructor handling in lint.py.

Tests verify that the linter correctly handles constructors with
constexpr/consteval/constinit modifiers that don't have an explicit
return type before the class name.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_len, assert_equal


class TestConstexprConstructor(TestCase):
    """Test cases for constexpr constructor detection."""

    def test_constexpr_default_constructor(self) -> None:
        """Test that constexpr default constructor is recognized as void."""
        content = """/**
 * @brief  Default constructor: creates an empty weak reference.
 * @throws None
 * @note   None
 * @warning None
 * @since  N/A
 * @ingroup none
 */
constexpr WeakPtr() noexcept = default;"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_constexpr_constructor_with_param(self) -> None:
        """Test that constexpr constructor with parameter is recognized as void."""
        content = """/**
 * @brief  Constructs an empty weak reference from nullptr.
 * @param[in] nullptr_t The nullptr literal.
 * @throws None
 * @note   None
 * @warning None
 * @since  N/A
 * @ingroup none
 */
constexpr WeakPtr(std::nullptr_t) noexcept {}"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_consteval_constructor(self) -> None:
        """Test that consteval constructor is recognized as void."""
        content = """/**
 * @brief  Compile-time constructor.
 * @param[in] value Initial value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         N/A
 * @ingroup       none
 */
consteval MyClass(int value);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_constinit_constructor(self) -> None:
        """Test that constinit constructor is recognized as void."""
        content = """/**
 * @brief  Constinit constructor.
 * @param[in] value Initial value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         N/A
 * @ingroup       none
 */
constinit MyClass(int value);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_inline_constructor(self) -> None:
        """Test that inline constructor is recognized as void."""
        content = """/**
 * @brief  Inline constructor.
 * @param[in] value Initial value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         N/A
 * @ingroup       none
 */
inline MyClass(int value);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_explicit_constructor(self) -> None:
        """Test that explicit constructor is recognized as void."""
        content = """/**
 * @brief  Explicit constructor.
 * @param[in] value Initial value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         N/A
 * @ingroup       none
 */
explicit MyClass(int value);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_static_member_function_returns_non_void(self) -> None:
        """Test that static member function returning non-void requires @return."""
        content = """/**
 * @brief  Gets the singleton instance.
 * @return Reference to the singleton instance.
 * @throws None
 * @note   Thread-safe.
 * @warning None
 * @since  1.0
 * @ingroup core
 */
static MyClass& get();"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        assert_len(violations, 0)

    def test_constexpr_non_constructor_requires_return(self) -> None:
        """Test that constexpr non-constructor function requires @return."""
        content = """/**
 * @brief  Computes a value.
 * @param[in] x Input value.
 * @throws        None
 * @note          None
 * @warning       None
 * @since         N/A
 * @ingroup       none
 */
constexpr int compute(int x);"""
        violations = lint.check_function_blocks(content, Path("test.h"))
        # Should have 1 violation for missing @return
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Non-void function missing @return")
