"""Tests for is_member_variable function in lint.py.

Tests verify that member variables are correctly distinguished from function declarations.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_true, assert_false


class TestIsMemberVariable(TestCase):
    """Test cases for the is_member_variable function."""

    def test_simple_member_variable(self) -> None:
        """Test simple member variable declaration."""
        content = "bool has_value_;"
        func_pos = content.find("has_value_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_member_variable_with_default(self) -> None:
        """Test member variable with default value."""
        content = "int count_ = 0;"
        func_pos = content.find("count_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_pointer_member_variable(self) -> None:
        """Test pointer member variable."""
        content = "float* ptr_;"
        func_pos = content.find("ptr_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_const_member_variable(self) -> None:
        """Test const member variable."""
        content = "const int kMaxSize = 100;"
        func_pos = content.find("kMaxSize")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_static_member_variable(self) -> None:
        """Test static member variable."""
        content = "static int instance_count_;"
        func_pos = content.find("instance_count_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_function_declaration(self) -> None:
        """Test that function declaration is NOT a member variable."""
        content = "int getValue();"
        func_pos = content.find("getValue")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_function_definition(self) -> None:
        """Test function definition with body."""
        content = "int getValue() { return value_; }"
        func_pos = content.find("getValue")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_const_function(self) -> None:
        """Test const member function."""
        content = "int getValue() const;"
        func_pos = content.find("getValue")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_function_pointer(self) -> None:
        """Test function pointer member variable."""
        content = "void (*callback_)();"
        func_pos = content.find("callback_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_std_function_member(self) -> None:
        """Test std::function member variable."""
        content = "std::function<void()> callback_;"
        func_pos = content.find("callback_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_member_with_comment_after_semicolon(self) -> None:
        """Test member with comment after semicolon."""
        content = "int value_;  // The value"
        func_pos = content.find("value_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_member_with_block_comment_after(self) -> None:
        """Test member with block comment after semicolon."""
        content = "int value_;  /* The value */"
        func_pos = content.find("value_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_function_returning_pointer(self) -> None:
        """Test function returning pointer."""
        content = "int* getPointer();"
        func_pos = content.find("getPointer")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_function_with_multiple_params(self) -> None:
        """Test function with multiple parameters."""
        content = "int compute(int a, int b);"
        func_pos = content.find("compute")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_function_with_template_params(self) -> None:
        """Test template function."""
        content = "template<typename T> T create();"
        func_pos = content.find("create")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_constexpr_function(self) -> None:
        """Test constexpr function."""
        content = "constexpr int size() { return 10; }"
        func_pos = content.find("size")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_noexcept_function(self) -> None:
        """Test noexcept function."""
        content = "void method() noexcept;"
        func_pos = content.find("method")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_function_with_trailing_return(self) -> None:
        """Test function with trailing return type."""
        content = "auto getValue() -> int;"
        func_pos = content.find("getValue")
        assert_false(lint.is_member_variable(content, func_pos))

    def test_array_member(self) -> None:
        """Test array member variable."""
        content = "int buffer_[1024];"
        func_pos = content.find("buffer_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_member_with_complex_type(self) -> None:
        """Test member with complex template type."""
        content = "std::map<std::string, int> lookup_table_;"
        func_pos = content.find("lookup_table_")
        assert_true(lint.is_member_variable(content, func_pos))

    def test_member_at_end_of_snippet(self) -> None:
        """Test member at end of content."""
        content = "int last_member_;"
        func_pos = content.find("last_member_")
        assert_true(lint.is_member_variable(content, func_pos))
