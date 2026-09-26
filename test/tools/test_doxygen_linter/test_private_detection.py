"""Tests for is_private_type and is_private_function in lint.py."""

from __future__ import annotations

import sys
from pathlib import Path

# Add scripts/doxygen to path to import lint module
sys.path.insert(0, str(Path(__file__).parent.parent.parent.parent / "scripts" / "doxygen"))

import lint

from test_framework import TestCase, assert_true, assert_false


class TestIsPrivateType(TestCase):
    """Test cases for the is_private_type function."""

    def test_public_section(self) -> None:
        """Test type in public section."""
        content = "class MyClass {\npublic:\n    enum class TestEnum { V1, };\n};"
        type_pos = content.find("TestEnum")
        assert_false(lint.is_private_type(content, type_pos))

    def test_private_section(self) -> None:
        """Test type in private section."""
        content = "class MyClass {\nprivate:\n    enum class TestEnum { V1, };\n};"
        type_pos = content.find("TestEnum")
        assert_true(lint.is_private_type(content, type_pos))

    def test_protected_section(self) -> None:
        """Test type in protected section."""
        content = "class MyClass {\nprotected:\n    enum class TestEnum { V1, };\n};"
        type_pos = content.find("TestEnum")
        # Protected is not private
        assert_false(lint.is_private_type(content, type_pos))

    def test_no_access_specifier_in_class(self) -> None:
        """Test type in class with no access specifier (defaults to private)."""
        content = "class MyClass {\n    enum class TestEnum { V1, };\n};"
        type_pos = content.find("TestEnum")
        # No explicit access specifier - defaults to private in a class
        assert_true(lint.is_private_type(content, type_pos))

    def test_no_access_specifier_in_struct(self) -> None:
        """Test type in struct with no access specifier (defaults to public)."""
        content = "struct MyStruct {\n    enum class TestEnum { V1, };\n};"
        type_pos = content.find("TestEnum")
        # No explicit access specifier - defaults to public in a struct
        assert_false(lint.is_private_type(content, type_pos))

    def test_multiple_access_sections(self) -> None:
        """Test with multiple access specifier changes."""
        content = """class MyClass {
public:
    struct PublicStruct { int x; };
private:
    struct PrivateStruct { int y; };
public:
    struct AnotherPublic { int z; };
};"""
        # First public struct
        pos = content.find("PublicStruct")
        assert_false(lint.is_private_type(content, pos))

        # Private struct
        pos = content.find("PrivateStruct")
        assert_true(lint.is_private_type(content, pos))

        # Second public struct
        pos = content.find("AnotherPublic")
        assert_false(lint.is_private_type(content, pos))

    def test_private_before_public(self) -> None:
        """Test private section before public."""
        content = """class MyClass {
private:
    struct PrivateStruct { int x; };
public:
    struct PublicStruct { int y; };
};"""
        pos = content.find("PublicStruct")
        assert_false(lint.is_private_type(content, pos))

    def test_nested_classes(self) -> None:
        """Test with nested class definitions."""
        content = """class Outer {
public:
    class Inner {
    private:
        struct DeepPrivate { int x; };
    public:
        struct DeepPublic { int y; };
    };
};"""
        pos = content.find("DeepPublic")
        # The last access specifier before DeepPublic is "public:"
        assert_false(lint.is_private_type(content, pos))

        pos = content.find("DeepPrivate")
        assert_true(lint.is_private_type(content, pos))


class TestIsPrivateFunction(TestCase):
    """Test cases for the is_private_function function."""

    def test_public_function(self) -> None:
        """Test function in public section."""
        content = "class MyClass {\npublic:\n    int publicFunc();\n};"
        func_pos = content.find("publicFunc")
        assert_false(lint.is_private_function(content, func_pos))

    def test_private_function(self) -> None:
        """Test function in private section."""
        content = "class MyClass {\nprivate:\n    int privateFunc();\n};"
        func_pos = content.find("privateFunc")
        assert_true(lint.is_private_function(content, func_pos))

    def test_protected_function(self) -> None:
        """Test function in protected section."""
        content = "class MyClass {\nprotected:\n    int protectedFunc();\n};"
        func_pos = content.find("protectedFunc")
        assert_false(lint.is_private_function(content, func_pos))

    def test_no_access_specifier(self) -> None:
        """Test function with no access specifier (defaults to private in class)."""
        content = "class MyClass {\n    int someFunc();\n};"
        func_pos = content.find("someFunc")
        # No explicit access specifier - defaults to private in a class
        assert_true(lint.is_private_function(content, func_pos))

    def test_namespace_function(self) -> None:
        """Test function at namespace scope (not a member)."""
        content = "namespace N {\nint globalFunc();\n}"
        func_pos = content.find("globalFunc")
        assert_false(lint.is_private_function(content, func_pos))

    def test_free_function(self) -> None:
        """Test free function (not in a class)."""
        content = "int freeFunc();"
        func_pos = content.find("freeFunc")
        assert_false(lint.is_private_function(content, func_pos))

    def test_access_specifier_order_variations(self) -> None:
        """Test various orderings of access specifiers."""
        # private -> public
        content = """class C {
private:
    int a();
public:
    int b();
};"""
        pos = content.find(" b()")
        assert_false(lint.is_private_function(content, pos))

        # protected -> private -> public
        content = """class C {
protected:
    int a();
private:
    int b();
public:
    int c();
};"""
        pos = content.find(" c()")
        assert_false(lint.is_private_function(content, pos))

        pos = content.find(" b()")
        assert_true(lint.is_private_function(content, pos))

    def test_colon_spacing_variants(self) -> None:
        """Test various spacing around access specifier colon."""
        content = "class C {\nprivate :\n    int func();\n};"
        func_pos = content.find("func")
        assert_true(lint.is_private_function(content, func_pos))
