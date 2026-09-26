"""Tests for check_enum_struct_docs function in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_len, assert_equal, assert_in


class TestCheckEnumStructDocs(TestCase):
    """Test cases for the check_enum_struct_docs function."""

    def test_documented_enum(self) -> None:
        """Test that documented enum passes."""
        content = """/**
 * @brief  A test enum.
 * @ingroup test
 */
enum class TestEnum {
    Value1,
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 0)

    def test_undocumented_enum(self) -> None:
        """Test that undocumented enum is detected."""
        content = "enum class TestEnum { Value1, };"
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].message, "Missing Doxygen comment")
        assert_equal(violations[0].symbol, "TestEnum")

    def test_documented_struct(self) -> None:
        """Test that documented struct passes."""
        content = """/**
 * @brief  A test struct.
 * @ingroup test
 */
struct TestStruct {
    int x;
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 0)

    def test_undocumented_struct(self) -> None:
        """Test that undocumented struct is detected."""
        content = "struct TestStruct { int x; };"
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "TestStruct")

    def test_documented_class(self) -> None:
        """Test that documented class passes."""
        content = """/**
 * @brief  A test class.
 * @ingroup test
 */
class TestClass {
public:
    int x;
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 0)

    def test_undocumented_class(self) -> None:
        """Test that undocumented class is detected."""
        content = "class TestClass { int x; };"
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "TestClass")

    def test_private_enum_is_skipped(self) -> None:
        """Test that private enum is skipped."""
        content = """/**
 * @brief  A test class.
 * @ingroup test
 */
class MyClass {
private:
    enum class PrivateEnum { Value1, };
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 0)

    def test_private_struct_is_skipped(self) -> None:
        """Test that private struct is skipped."""
        content = """/**
 * @brief  A test class.
 * @ingroup test
 */
class MyClass {
private:
    struct PrivateStruct { int x; };
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 0)

    def test_public_enum_is_checked(self) -> None:
        """Test that public enum is checked."""
        content = """/**
 * @brief  A test class.
 * @ingroup test
 */
class MyClass {
public:
    enum class PublicEnum { Value1, };
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "PublicEnum")

    def test_union_is_checked(self) -> None:
        """Test that union is checked."""
        content = "union TestUnion { int a; float b; };"
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "TestUnion")

    def test_multiple_undocumented_types(self) -> None:
        """Test that multiple undocumented types are all detected."""
        content = """enum class Enum1 { V1, };
struct Struct1 { int x; };
class Class1 { int y; };"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 3)

    def test_enum_with_line_comment(self) -> None:
        """Test enum with line-style comment."""
        content = """/// @brief A test enum.
/// @ingroup test
enum class TestEnum {
    Value1,
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 0)

    def test_struct_with_export_macro(self) -> None:
        """Test struct with export macro is checked."""
        content = """CF_UI_EXPORT struct ExportedStruct {
    int x;
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "ExportedStruct")

    def test_class_with_dll_export(self) -> None:
        """Test class with DLL export macro."""
        content = """class DLL_EXPORT ExportedClass {
public:
    int x;
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "ExportedClass")

    def test_declspec_macro(self) -> None:
        """Test with __declspec macro."""
        content = """struct __declspec(dllexport) ExportedStruct {
    int x;
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
        assert_equal(violations[0].symbol, "ExportedStruct")

    def test_nested_access_sections(self) -> None:
        """Test nested class with different access sections."""
        content = """/**
 * @brief  An outer class.
 * @ingroup test
 */
class Outer {
public:
    struct PublicStruct { int x; };

private:
    struct PrivateStruct { int y; };

protected:
    enum class ProtectedEnum { Value1, };
};"""
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        # PublicStruct and ProtectedEnum should be detected
        # PrivateStruct should be skipped
        assert_len(violations, 2)

    def test_default_access_section(self) -> None:
        """Test types with no explicit access specifier."""
        # class default is private - but at namespace level, check it (public API)
        content = "class DefaultClass { int x; };"
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)

        # struct default is public - should be checked
        content = "struct DefaultStruct { int x; };"
        violations = lint.check_enum_struct_docs(content, Path("test.h"))
        assert_len(violations, 1)
