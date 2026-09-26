"""Tests for @code ... @endcode block detection in lint.py.

Tests verify that the linter correctly ignores symbols within
@code ... @endcode blocks, as these are example code, not actual definitions.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_true, assert_false, assert_len


class TestIsInCodeBlock(TestCase):
    """Test cases for the is_in_code_block function."""

    def test_position_before_code_block(self) -> None:
        """Test that position before @code block returns False."""
        content = """
/**
 * @brief Some function.
 * @code
 * int example() { return 0; }
 * @endcode
 */
int actualFunction();
"""
        # Position of actualFunction is after @code block
        pos = content.find("actualFunction")
        assert_false(lint.is_in_code_block(pos, content))

    def test_position_inside_code_block(self) -> None:
        """Test that position inside @code block returns True."""
        content = """
/**
 * @brief Some function.
 * @code
 * int example() { return 0; }
 * @endcode
 */
"""
        # Position of example is inside @code block
        pos = content.find("example")
        assert_true(lint.is_in_code_block(pos, content))

    def test_position_after_code_block(self) -> None:
        """Test that position after @endcode returns False."""
        content = """
/**
 * @brief Some function.
 * @code
 * int example() { return 0; }
 * @endcode
 */
int actualFunction();
"""
        # Position of actualFunction is after @endcode
        pos = content.find("actualFunction")
        assert_false(lint.is_in_code_block(pos, content))

    def test_multiple_code_blocks(self) -> None:
        """Test handling multiple @code blocks."""
        content = """
/**
 * @brief Function with multiple examples.
 * @code
 * int first() { return 1; }
 * @endcode
 * @code
 * int second() { return 2; }
 * @endcode
 */
"""
        # Position of first is in first @code block
        pos_first = content.find("first")
        assert_true(lint.is_in_code_block(pos_first, content))

        # Position of second is in second @code block
        pos_second = content.find("second")
        assert_true(lint.is_in_code_block(pos_second, content))

    def test_htmlonly_block(self) -> None:
        """Test that @htmlonly blocks are also detected."""
        content = """
/**
 * @brief Function with HTML example.
 * @htmlonly
 * Some example content
 * @endhtmlonly
 */
"""
        pos = content.find("example content")
        assert_true(lint.is_in_code_block(pos, content))

    def test_verbatim_block(self) -> None:
        """Test that @verbatim blocks are also detected."""
        content = """
/**
 * @brief Function with verbatim example.
 * @verbatim
 * example text here
 * @endverbatim
 */
"""
        pos = content.find("example text")
        assert_true(lint.is_in_code_block(pos, content))

    def test_no_code_block(self) -> None:
        """Test content without any @code blocks."""
        content = """
/**
 * @brief Some function.
 */
int actualFunction();
"""
        pos = content.find("actualFunction")
        assert_false(lint.is_in_code_block(pos, content))


class TestCodeBlockInFunctionDetection(TestCase):
    """Test that functions in @code blocks are not flagged."""

    def test_function_in_code_block_not_flagged(self) -> None:
        """Test that example functions in @code blocks are ignored."""
        content = """/**
 * @brief A class with example usage.
 *
 * Example usage:
 * @code
 * MyClass obj;
 * obj.process();
 * @endcode
 */
class MyClass {
    /**
     * @brief  Process data.
     * @return        Result.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    int process();
};"""
        violations = lint.check_function_blocks(content, Path("test.hpp"))
        # Only the actual process() method should be found
        # The example obj.process() in @code should be ignored
        assert_len(violations, 0)

    def test_class_example_in_code_block_not_flagged(self) -> None:
        """Test that example class names in @code blocks are ignored."""
        content = """/**
 * @brief  Template class for lazy initialization.
 *
 * @code
 * class MyInitializer : public cf::CallOnceInit<MyData> {
 * protected:
 *     bool init_resources() override {
 *         return true;
 *     }
 * };
 * @endcode
 */
template <typename T> struct CallOnceInit {
    /**
     * @brief  Get the resource.
     * @return        Reference to resource.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    T& get();
};"""
        violations = lint.check_function_blocks(content, Path("test.hpp"))
        # The example MyInitializer and init_resources in @code should be ignored
        # Only the actual get() method should be checked (and it has all tags)
        assert_len(violations, 0)

    def test_variable_example_in_code_block_not_flagged(self) -> None:
        """Test that example variable names in @code blocks are ignored."""
        content = """/**
 * @brief  RAII scope guard.
 *
 * @code
 * cf::ScopeGuard guard([&f]() { fclose(f); });
 * @endcode
 *
 * @ingroup base_utilities
 */
class ScopeGuard {
    /**
     * @brief  Dismisses the guard.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    void dismiss();
};"""
        violations = lint.check_function_blocks(content, Path("test.hpp"))
        # The example 'guard' variable in @code should be ignored
        assert_len(violations, 0)


class TestCodeBlockInTypeDetection(TestCase):
    """Test that types in @code blocks are not flagged."""

    def test_type_in_code_block_not_flagged(self) -> None:
        """Test that example types in @code blocks are ignored."""
        content = """/**
 * @file test.hpp
 * @brief Test file.
 */

/**
 * @brief  A color class.
 *
 * @code
 * CFColor color("#FF0000");
 * @endcode
 */
class CFColor {
    /**
     * @brief  A test struct.
     */
    struct InnerStruct {
        int value;
    };
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.hpp"))
        # The example 'color' variable usage in @code should be ignored
        # Only CFColor and InnerStruct should be checked
        # InnerStruct has documentation, so no violations
        assert_len(violations, 0)
